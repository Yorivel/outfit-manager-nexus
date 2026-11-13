// Character look (#35): an outfit brings a snapshot of the character's barber look (hairstyle, beard, colours,
// tattoos, makeup, every barber option) along while it is applied; outfits without one show the No Outfit look.
//
// The game (2.05, build 26636972) keeps a character's look as {vec<u8> 16 mesh options, vec<u8> 250 values, u32 1}
// (the save's CustomizationSaveData, 0xFF = the default): the actor's +0x68 -> +0x20 component holds the shown one at
// +0x3B0. The barber's reply handler (TrocTrCharacterCustomizationAck, +CBA740) applies a look by building one
// (+192B2B0, 0xFF-filled), filling it, calling +1933070(component, &result, &look, 0) and freeing it (+41D270); OM does
// the same, on the HUD frame (tested in game 2026-11-13). That only changes what is shown: the game's save keeps the
// look the barber last confirmed.
//
// Per character (bound to saves with the outfits): look_base = the No Outfit look (the character's own). The look shown
// is the applied outfit's snapshot, else look_base; it is put on again after every load and character switch (the game
// builds the character from its saved look). The barber (2026-11-13, agreed): its reply is hooked; after leaving it, a
// change made while an outfit with a look was applied asks once - save it to that outfit, or make it the character's own
// look (the outfit's comes back). A change without one is the character's own look.
#include "charlook.h"
#include "addrs.h"
#include "gear.h"
#include "lang.h"
#include "log.h"
#include "looks.h"
#include "mem.h"
#include "prompts.h"
#include "store.h"
#include "wheel_hooks.h"

#include <MinHook.h>
#include <atomic>
#include <map>
#include <mutex>
#include <cstring>
#include <intrin.h>

namespace om {

std::atomic<U64> g_hud_root{0}; // (the status gauge's UI root, from its frame: the barber question opens from it)

namespace {

// (the game's look functions and the record's offset: addrs.cpp, found by the resolver after a game update)
constexpr U32 kMeshCount = 16, kValueCount = 250;
constexpr size_t kLookBytes = kMeshCount + kValueCount;

struct Vec {
    std::uint8_t* p;
    U32 n, cap;
};
struct GameLook {
    Vec mesh, values;
    U32 one, pad;
};
using LookFn = void(__fastcall*)(GameLook*);
using ApplyFn = U32*(__fastcall*)(U64 comp, U32* result, GameLook* look, U64 zero);
using AckFn = U64(__fastcall*)(U64, U64, U64, U64);
AckFn g_ack_orig = nullptr;
thread_local bool t_own_apply = false; // (OM's own apply, or the barber's reply: the loading screen hook lets it pass)

// The game's functions are the ones this was made for (else the feature stays off: a game update moved them).
bool usable()
{
    static int ok = -1;
    if (ok < 0) {
        unsigned char j = 0;
        ok = have({kRvaLookCtor, kRvaLookDtor, kRvaLookApply, kOffCompLook}) && rd(g_base + kRvaLookApply, j) &&
             (j == 0xE9 || j == 0x48);
        LOG("character look: %s", ok ? "on" : "off (the game's look functions weren't found)");
    }
    return ok == 1;
}

U64 look_component(U64 actor) { return rp(rp(actor + kOffActorComp) + 0x20); }

bool read_vec(U64 at, Vec& v)
{
    U64 p = 0;
    if (!rd(at, p) || !rd(at + 8, v.n) || !rd(at + 12, v.cap) || !plausible(p)) return false;
    v.p = reinterpret_cast<std::uint8_t*>(p);
    return true;
}

std::string read_look(U64 actor)
{
    const U64 look = actor ? rp(look_component(actor) + kOffCompLook) : 0;
    Vec m{}, v{};
    if (!look || !read_vec(look, m) || !read_vec(look + 0x10, v) || m.n != kMeshCount || v.n != kValueCount) return {};
    std::string out(kLookBytes, '\0');
    for (U32 i = 0; i < kLookBytes; ++i) {
        std::uint8_t b = 0;
        if (!rd(reinterpret_cast<U64>(i < kMeshCount ? m.p + i : v.p + (i - kMeshCount)), b)) return {};
        out[i] = static_cast<char>(b);
    }
    return out;
}

bool apply_look(U64 actor, const std::string& want)
{
    const U64 comp = look_component(actor);
    if (!comp || want.size() != kLookBytes || !usable()) return false;
    GameLook look{};
    reinterpret_cast<LookFn>(g_base + kRvaLookCtor)(&look);
    bool ok = look.mesh.n == kMeshCount && look.values.n == kValueCount && look.mesh.p && look.values.p;
    if (ok) {
        std::memcpy(look.mesh.p, want.data(), kMeshCount);
        std::memcpy(look.values.p, want.data() + kMeshCount, kValueCount);
        U32 result = 0;
        t_own_apply = true; // (the loading screen hook lets OM's own call pass)
        __try {
            reinterpret_cast<ApplyFn>(g_base + kRvaLookApply)(comp, &result, &look, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        t_own_apply = false;
        LOG("character look: applied (result %u)%s", result, ok ? "" : " - FAILED (an exception)");
    } else {
        LOG("character look: the game's look has %u / %u entries, not %u / %u - nothing applied", look.mesh.n,
            look.values.n, kMeshCount, kValueCount);
    }
    reinterpret_cast<LookFn>(g_base + kRvaLookDtor)(&look);
    return ok;
}

std::string brief(const std::string& l) // (hairstyle, beard, hair colour: for the log)
{
    if (l.size() != kLookBytes) return "(none)";
    char b[48];
    snprintf(b, sizeof b, "hair %u beard %u colour %u", std::uint8_t(l[2]), std::uint8_t(l[3]), std::uint8_t(l[kMeshCount + 6]));
    return b;
}

// Pending work for the HUD frame: the look to follow the applied outfit (after an outfit change, a load, a character
// switch); the barber's change to sort out (once out of it).
U16 g_follow_char = kNoCharacter;
U64 g_follow_at = 0, g_follow_wait = 0;
std::atomic<U64> g_barber_at{0};
std::string g_barber_look; // (the change asked about; its outfit's name: g_barber_outfit)
std::string g_barber_outfit;

void schedule(U16 character, U64 wait)
{
    g_follow_char = character, g_follow_at = GetTickCount64(), g_follow_wait = wait;
}

// The look the character should show: the applied outfit's snapshot, else its own (No Outfit) look.
std::string wanted(U16 character, const std::string& live, const char** what)
{
    const int a = anchor_outfit();
    const Outfit* o = a >= 0 ? &g_outfits[size_t(a)] : nullptr;
    if (o && charlook_on(*o)) {
        *what = "its snapshot";
        return o->look_snap;
    }
    std::lock_guard<std::mutex> lock(g_store_mu);
    CharacterOutfits& c = store_for(character);
    if (c.look_base.size() != kLookBytes) c.look_base = live; // (first seen: the look now is the character's own)
    *what = "the character's own look";
    return c.look_base;
}

void follow(U64 actor, U16 character)
{
    const std::string live = read_look(actor);
    if (live.empty()) { // (right after a character switch the game fills it in a little later)
        static int tries = 0;
        if (++tries <= 30) {
            schedule(character, 1000);
            return;
        }
        tries = 0;
        LOG("character look: can't read character %u's look - left as it is", character);
        return;
    }
    const char* what = "";
    const std::string want = wanted(character, live, &what);
    const int a = anchor_outfit();
    if (want == live) return;
    LOG("character look: %s -> %s (%s)", a >= 0 ? ("\"" + g_outfits[size_t(a)].name + "\"").c_str() : "no outfit", what,
        brief(want).c_str());
    apply_look(actor, want);
    save_outfits();
}

// After the barber (its reply seen, now out of it): a change made while an outfit with a look was applied asks where it
// goes; else it is the character's own look.
void barber_done(U64 actor, U16 character)
{
    const std::string live = read_look(actor);
    if (live.empty()) return;
    const int a = anchor_outfit();
    Outfit* o = a >= 0 ? &g_outfits[size_t(a)] : nullptr;
    if (o && charlook_on(*o)) {
        if (live == o->look_snap) return; // (nothing changed)
        g_barber_look = live, g_barber_outfit = o->name;
        const U64 root = g_hud_root.load();
        const std::string msg = lang::f("ask.barber_look", {{"outfit", o->name}});
        if (!root || !open_barber_question(root, lang::t("setting.look"), msg.c_str(),
                                           lang::f("ask.yes.barber_outfit", {{"outfit", o->name}}).c_str(),
                                           lang::t("ask.no.barber_own"))) {
            LOG("character look: the barber question didn't open - the change is the character's own look");
            charlook_barber_answer(false);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        store_for(character).look_base = live;
    }
    LOG("character look: the barber's change is the character's own look (%s)", brief(live).c_str());
    save_outfits();
}

// From the loading screen (agreed 2026-11-13): a load hands the played character's component its saved look through
// the apply (+1933070 -> +B767BF0, seen in a probe: half a second after the save is read, on the main thread). For a
// while after a load that call gets the look OM wants instead (the applied outfit's snapshot, else the No Outfit look,
// from the loaded set), so the character is built with it; OM's own calls, other threads and other characters pass.
// (the apply is a jump into protected code: its target, the body, is read from the jump - +B767BF0 on 2.05)
U64 apply_body()
{
    unsigned char op = 0;
    std::int32_t rel = 0;
    if (!rd(g_base + kRvaLookApply, op)) return 0;
    if (op != 0xE9) return kRvaLookApply; // (a plain function: hooked itself)
    if (!rd(g_base + kRvaLookApply + 1, rel)) return 0;
    return U64(std::int64_t(kRvaLookApply) + 5 + rel);
}
ApplyFn g_apply_orig = nullptr;
std::atomic<U64> g_swap_until{0};
std::atomic<U32> g_swap_gen{0xFFFFFFFF};

// The look a loaded save wants for a character: its applied outfit's snapshot, else its No Outfit look ("" unknown).
std::string load_want(U16 character, bool* busy = nullptr)
{
    std::unique_lock<std::mutex> lock(g_store_mu, std::defer_lock);
    if (busy) { // (try only: the caller may run while another part of OM holds the outfits)
        *busy = !lock.try_lock();
        if (*busy) return {};
    } else {
        lock.lock();
    }
    for (const CharacterOutfits& c : g_published) {
        if (c.character != character) continue;
        for (const Outfit& o : c.outfits)
            if (o.worn && charlook_on(o)) return o.look_snap;
        return c.look_base.size() == kLookBytes ? c.look_base : std::string();
    }
    return {};
}

U32* __fastcall apply_hook(U64 comp, U32* result, GameLook* look, U64 zero)
{
    const U64 now = GetTickCount64();
    if (const U32 gen = g_generation.load(); gen != g_swap_gen.load()) g_swap_gen = gen, g_swap_until = now + 90000;
    if (t_own_apply || now > g_swap_until.load() || (g_ui_thread && GetCurrentThreadId() != g_ui_thread) || !look)
        return g_apply_orig(comp, result, look, zero);
    U64 ctx[4] = {};
    const U64 actor = acquire_player(ctx);
    const U16 ch = actor && look_component(actor) == comp ? character_of(actor) : kNoCharacter;
    if (ctx[0]) release_player(ctx);
    if (ch == kNoCharacter) return g_apply_orig(comp, result, look, zero);
    g_swap_until = 0; // (the played character's build after the load: once)
    const std::string want = load_want(ch);
    if (want.size() != kLookBytes || look->mesh.n != kMeshCount || look->values.n != kValueCount || !look->mesh.p ||
        !look->values.p)
        return g_apply_orig(comp, result, look, zero);
    std::string given(reinterpret_cast<const char*>(look->mesh.p), kMeshCount);
    given.append(reinterpret_cast<const char*>(look->values.p), kValueCount);
    if (given == want) return g_apply_orig(comp, result, look, zero);
    GameLook mine{}; // (the game's look left as it is: OM's own copy goes in its place)
    reinterpret_cast<LookFn>(g_base + kRvaLookCtor)(&mine);
    if (mine.mesh.n != kMeshCount || mine.values.n != kValueCount || !mine.mesh.p || !mine.values.p) {
        reinterpret_cast<LookFn>(g_base + kRvaLookDtor)(&mine);
        return g_apply_orig(comp, result, look, zero);
    }
    std::memcpy(mine.mesh.p, want.data(), kMeshCount);
    std::memcpy(mine.values.p, want.data() + kMeshCount, kValueCount);
    LOG("character look: character %u built with OM's look from the loading screen (%s; the save had %s)", ch,
        brief(want).c_str(), brief(given).c_str());
    U32* r = g_apply_orig(comp, result, &mine, zero);
    reinterpret_cast<LookFn>(g_base + kRvaLookDtor)(&mine);
    return r;
}

U64 __fastcall ack_hook(U64 a, U64 b, U64 c, U64 d)
{
    t_own_apply = true; // (the barber's change: never swapped)
    const U64 r = g_ack_orig(a, b, c, d);
    t_own_apply = false;
    g_barber_at = GetTickCount64(); // (the barber confirmed a change: sorted out once out of it)
    return r;
}

} // namespace

bool charlook_on(const Outfit& o) { return o.char_look == 1 && o.look_snap.size() == kLookBytes; }

std::string charlook_read()
{
    U64 ctx[4] = {};
    const U64 actor = acquire_player(ctx);
    std::string l = actor ? read_look(actor) : std::string();
    if (ctx[0]) release_player(ctx);
    return l;
}

// A function start the compiler writes (mov [rsp+..], reg / push / sub rsp / mov rax, rsp ...): the unpacked code.
bool looks_like_code(const unsigned char* b)
{
    return (b[0] == 0x48 && (b[1] == 0x89 || b[1] == 0x8B || b[1] == 0x83)) || (b[0] == 0x40 && b[1] >= 0x53 && b[1] <= 0x57) ||
           (b[0] == 0x4C && b[1] == 0x8B) || (b[0] >= 0x53 && b[0] <= 0x57) || (b[0] == 0x41 && b[1] >= 0x54 && b[1] <= 0x57);
}

DWORD WINAPI hook_body_when_unpacked(LPVOID)
{
    unsigned char last[16] = {};
    for (int i = 0; i < 1200; ++i, Sleep(100)) { // (up to 2 minutes; the game unpacks within seconds of starting)
        const U64 body = apply_body();
        unsigned char now[16] = {};
        bool read = body != 0;
        for (int k = 0; read && k < 16; ++k) read = rd(g_base + body + k, now[k]);
        if (!read || !looks_like_code(now) || std::memcmp(now, last, 16) != 0) {
            std::memcpy(last, now, 16);
            continue;
        }
        void* st = reinterpret_cast<void*>(g_base + body);
        if (MH_CreateHook(st, reinterpret_cast<void*>(&apply_hook), reinterpret_cast<void**>(&g_apply_orig)) != MH_OK ||
            MH_EnableHook(st) != MH_OK) {
            LOG("character look: loading screen hook at +%llX failed - the look follows once in the game", body);
            return 0;
        }
        LOG("character look: loading screen hook in at +%llX (after %d ms)", body, i * 100);
        // (the game writes that code again later - a second unpacking or its integrity check - which took the hook out:
        // kept watching, the hook goes back in whenever its jump is gone)
        int again = 0;
        for (;; Sleep(200)) {
            unsigned char op = 0;
            if (!rd(g_base + body, op) || op == 0xE9) continue;
            MH_RemoveHook(st);
            if (MH_CreateHook(st, reinterpret_cast<void*>(&apply_hook), reinterpret_cast<void**>(&g_apply_orig)) != MH_OK ||
                MH_EnableHook(st) != MH_OK) {
                LOG("character look: the game wrote over the loading screen hook and it couldn't go back in");
                return 0;
            }
            if (++again <= 20) LOG("character look: the game wrote over the loading screen hook; it is back in (%d)", again);
        }
    }
    LOG("character look: the look apply never looked unpacked - the look follows once in the game");
    return 0;
}

void charlook_built(U64 actor, U16 character)
{
    // (a load builds the character from the save's look - 40 s before the HUD, and OM's fallback, start: the right look
    // goes on as the character is built, where its visibility looks do; once per character and load)
    if (!actor || character == kNoCharacter || !usable()) return;
    { // (the played character only: companions are built too)
        U64 ctx[4] = {};
        const U64 player = acquire_player(ctx);
        if (ctx[0]) release_player(ctx);
        if (player != actor) return;
    }
    static std::mutex mu;
    static std::map<U16, U32> done;
    const U32 gen = g_generation.load();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (done.count(character) && done[character] == gen) return;
    }
    const std::string live = read_look(actor);
    if (live.empty()) return; // (not there yet: the next build)
    bool busy = false;
    const std::string want = load_want(character, &busy);
    if (busy) return; // (the outfits are busy: the next build)
    {
        std::lock_guard<std::mutex> lock(mu);
        done[character] = gen;
    }
    if (want.size() != kLookBytes || want == live) return;
    LOG("character look: character %u built: OM's look on it (%s; the save had %s)", character, brief(want).c_str(),
        brief(live).c_str());
    apply_look(actor, want);
}

void charlook_install()
{
    if (!usable()) return;
    // (the look from the loading screen: the apply's body is protected code the game unpacks after it starts - hooked
    // before that, the unpacking wrote over the hook and loads kept the save's look, 0.7.0. So a thread waits until
    // the body reads as a function start, the same twice in a row, then hooks it)
    CloseHandle(CreateThread(nullptr, 0, &hook_body_when_unpacked, nullptr, 0, nullptr));
    if (!have({kRvaBarberAck})) {
        LOG("character look: the barber's reply wasn't found - barber changes are the character's own look");
        return;
    }
    void* t = reinterpret_cast<void*>(g_base + kRvaBarberAck);
    if (MH_CreateHook(t, reinterpret_cast<void*>(&ack_hook), reinterpret_cast<void**>(&g_ack_orig)) != MH_OK ||
        MH_EnableHook(t) != MH_OK)
        LOG("character look: barber hook at +%llX failed", kRvaBarberAck);
}

void charlook_follow()
{
    if (g_character == kNoCharacter || !usable()) return;
    schedule(g_character, 0);
}

void charlook_tick()
{
    if (!usable()) return;
    const U64 now = GetTickCount64();
    { // (a load or a character switch: the character is built from its saved look - the right one goes on again)
        static U32 seen_gen = 0xFFFFFFFF;
        static U16 seen_char = kNoCharacter;
        const U32 gen = g_generation.load();
        if (gen != seen_gen || g_character != seen_char) {
            seen_gen = gen, seen_char = g_character;
            if (g_character != kNoCharacter) schedule(g_character, 500);
        }
    }
    // (on the HUD frame again: out of the barber - its change sorted out, a moment later)
    if (const U64 at = g_barber_at.load(); at && now - at > 800 && !look_changing()) {
        g_barber_at = 0;
        U64 ctx[4] = {};
        const U64 actor = acquire_player(ctx);
        if (actor && character_of(actor) == g_character) barber_done(actor, g_character);
        if (ctx[0]) release_player(ctx);
        return;
    }
    if (g_follow_char == kNoCharacter || now - g_follow_at < g_follow_wait) return;
    // (after the outfit's gear and its effect: one model rebuild at a time; given up waiting after 8 s)
    if ((look_changing() || fx_transition_busy()) && now - g_follow_at < g_follow_wait + 8000) return;
    const U16 c = g_follow_char;
    g_follow_char = kNoCharacter;
    U64 ctx[4] = {};
    const U64 actor = acquire_player(ctx);
    if (actor && character_of(actor) == c && c == g_character) follow(actor, c);
    if (ctx[0]) release_player(ctx);
}

void charlook_barber_answer(bool to_outfit)
{
    const std::string live = g_barber_look;
    g_barber_look.clear();
    if (live.size() != kLookBytes) return;
    Outfit* o = nullptr;
    for (Outfit& x : g_outfits)
        if (x.name == g_barber_outfit) o = &x;
    if (to_outfit && o) {
        o->look_snap = live;
        LOG("character look: the barber's change saved to \"%s\" (%s)", o->name.c_str(), brief(live).c_str());
    } else {
        {
            std::lock_guard<std::mutex> lock(g_store_mu);
            store_for(g_character).look_base = live;
        }
        LOG("character look: the barber's change is the character's own look (%s); the outfit's look again",
            brief(live).c_str());
        charlook_follow(); // (the applied outfit's own look goes back on)
    }
    save_outfits();
}

bool charlook_take(Outfit& o, bool applied)
{
    (void)applied;
    if (!usable()) return false;
    const std::string live = charlook_read();
    if (live.empty()) {
        LOG("character look: \"%s\": can't read the character's look - no snapshot", o.name.c_str());
        return false;
    }
    o.look_snap = live, o.char_look = 1;
    LOG("character look: \"%s\": snapshot taken (%s)", o.name.c_str(), brief(live).c_str());
    return true;
}

void charlook_off(Outfit& o, bool applied)
{
    o.char_look = 0;
    LOG("character look: \"%s\": off (its snapshot kept)", o.name.c_str());
    if (applied) charlook_follow(); // (the character's own look comes back)
}

} // namespace om
