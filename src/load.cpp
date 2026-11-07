// Loading an outfit: equip steps, the load pump, verification and the loss check.
#include "om.h"
#include "load.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "store.h"
#include "saves.h"
#include "looks.h"
#include "stash.h"
#include "prompts.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"

namespace om {

// --- Loading an outfit --------------------------------------------------------------------------
// All at once on Space, with the client-side requests the inventory screen uses (they apply immediately):
// first the worn pieces that must come off (slots empty in the outfit, pieces the outfit wears in another
// slot), then every slot whose worn piece differs. The equip request names the piece by bag id + position,
// looked up right before each request (positions change as pieces come off).
struct LoadStep {
    bool equip;
    U16 slot; // equip slot
    U16 key;
    U64 uid;
};

// What the last guarded equip / unequip call faulted on (filled by the exception filter, logged by error_text).
struct Fault {
    U32 code;
    U64 at, info[2];
    U64 rip, rsp, rax, rcx, rdx, r8;
    U64 frames[12]; // values on the stack that point into the game image (return addresses)
    int frame_count;
};
Fault g_fault;

U64 image_size()
{
    static U64 size = 0;
    if (!size && g_base) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        size = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    }
    return size;
}

int note_fault(EXCEPTION_POINTERS* ep)
{
    g_fault = {};
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* cx = ep->ContextRecord;
    g_fault.code = er->ExceptionCode;
    g_fault.at = reinterpret_cast<U64>(er->ExceptionAddress);
    for (DWORD i = 0; i < 2 && i < er->NumberParameters; ++i) g_fault.info[i] = er->ExceptionInformation[i];
    g_fault.rip = cx->Rip, g_fault.rsp = cx->Rsp, g_fault.rax = cx->Rax, g_fault.rcx = cx->Rcx;
    g_fault.rdx = cx->Rdx, g_fault.r8 = cx->R8;
    U64 end = g_base + image_size();
    for (int i = 0; i < 96 && g_fault.frame_count < 12; ++i) {
        U64 v = 0;
        if (rd(cx->Rsp + i * 8ull, v) && v > g_base && v < end) g_fault.frames[g_fault.frame_count++] = v;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// "+RVA" for game addresses, "module+offset" for others.
std::string where(U64 a)
{
    char b[96];
    if (g_base && a >= g_base && a < g_base + image_size()) {
        std::snprintf(b, sizeof b, "+%llX", a - g_base);
        return b;
    }
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &m) &&
        m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(b, sizeof b, "%s+%llX", base ? base + 1 : name, a - reinterpret_cast<U64>(m));
    } else {
        std::snprintf(b, sizeof b, "%llX", a);
    }
    return b;
}

std::string fault_text()
{
    char b[256];
    std::snprintf(b, sizeof b, "faulted: code %08X at %s (info %llX %llX) rsp %llX rax %llX rcx %llX rdx %llX r8 %llX",
                  g_fault.code, where(g_fault.at).c_str(), g_fault.info[0], g_fault.info[1], g_fault.rsp,
                  g_fault.rax, g_fault.rcx, g_fault.rdx, g_fault.r8);
    std::string s = b;
    if (g_fault.frame_count) s += "\n      stack:";
    for (int i = 0; i < g_fault.frame_count; ++i) s += " " + where(g_fault.frames[i]);
    return s;
}

int call_unequip(U64 inv, U16 slot)
{
    int err = kFault;
    if (!g_unequip) return err;
    __try {
        g_unequip(inv, &err, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}
CompanionEquipFn g_companion_equip_orig = nullptr;
int g_bag_override = -1; // the bag for our running call (UI thread only; -1: none)

U64 __fastcall companion_equip_hook(U64 target, U64 err, U64 id, U64 bag, U64 pos, U64 slot)
{
    if (g_bag_override >= 0 && GetCurrentThreadId() == g_ui_thread) {
        LOG("  (companion request: bag %u -> %d)", U32(bag & 0xFFFF), g_bag_override);
        bag = (bag & ~0xFFFFull) | U16(g_bag_override);
    }
    return g_companion_equip_orig(target, err, id, bag, pos, slot);
}

bool companion_pull_ok() { return g_companion_equip_orig != nullptr; }

int call_equip_request_raw(U64 actor, U64 item, short pos, U16 slot)
{
    int err = kFault;
    if (!g_equip_request) return err;
    alignas(16) unsigned char copy[0xd0];
    __try {
        std::memcpy(copy, reinterpret_cast<const void*>(item), 200);
        *reinterpret_cast<U16*>(copy + 0xc8) = 0xFFFF;
        *reinterpret_cast<short*>(copy + 0xca) = pos;
        g_equip_request(actor, &err, copy, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

// bag: where the piece is; storage / the wardrobe go through the bag override (carried bags need none).
int call_equip_request(U64 actor, U64 item, short pos, U16 slot, U16 bag)
{
    bool pull = (bag == kBagStorage || bag == kBagWardrobe) && g_companion_equip_orig;
    if (pull) g_bag_override = bag;
    int err = call_equip_request_raw(actor, item, pos, slot);
    g_bag_override = -1;
    return err;
}

int call_unequip_request(U64 actor, U16 slot)
{
    int err = kFault;
    if (!g_unequip_request) return err;
    __try {
        g_unequip_request(actor, &err, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

int call_equip(U64 inv, U16 bag, short pos, U16 slot)
{
    int err = kFault;
    if (!g_equip) return err;
    __try {
        g_equip(inv, &err, bag, pos, static_cast<short>(slot), 0);
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

U64 call_inventory(U64 actor)
{
    if (!g_inventory) return 0;
    __try {
        return g_inventory(actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

U64 error_entry(int err)
{
    if (!g_error_text || !g_error_mgr) return 0;
    __try {
        return g_error_text(g_error_mgr(), &err);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string error_text(int err)
{
    if (err == 0) return "ok";
    char b[48];
    if (err == kFault) return fault_text();
    std::snprintf(b, sizeof b, "error %X ", static_cast<U32>(err));
    U64 e = error_entry(err);
    return b + (e ? cstr(rp(rp(e))) : std::string("(no text)"));
}

// Runs one request; false if there is no character to run it for. Kliff (kind 1): the client-side functions (they
// apply at once); companions: the game's UI requests. g_step_err: the last put-on's answer (0 = ok).

int g_step_err = 0;
int g_steps_ok = 0; // requests that went through (counted per load)

bool run_step(const LoadStep& s, size_t number)
{
    g_step_err = 0;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 c = actor ? rp(actor + kOffActorComp) : 0;
    U64 inv = c ? rp(c + kOffCompInventory) : 0;
    U64 kind = actor ? rp(actor + kOffActorKind) : 0;
    bool player = inv && kind && (r32(kind + 1) & 0xFF) == 1;
    // Kliff through the game's equip requests too, as its own menu does, not the client-side functions: with a stacking
    // mod installed ("Desert Link Unlimited Stacks"), the direct take-off lost the piece - it never reached the bag;
    // through the requests nothing was lost (reproduced 2026-11-07, #1). `kliff` keeps his bag rules.
    bool kliff = player;
    if (player && g_equip_request && g_unequip_request) player = false;
    bool companion = actor && kind && !player && g_equip_request && g_unequip_request;
    bool ok = player || companion;
    if (!ok) {
        LOG("  step %zu: no character to change (actor %llX, kind %u, inventory %llX)", number, actor,
            kind ? r32(kind + 1) & 0xFF : 0xFF, inv);
    } else if (companion && !s.equip) {
        int err = call_unequip_request(actor, s.slot);
        g_steps_ok += err == 0; // (a change went through: a model swap follows)
        LOG("  step %zu: (companion) take off equipment-%u (key %u uid %llX): %s", number, s.slot, s.key, s.uid,
            error_text(err).c_str());
    } else if (companion) {
        U64 mgr = call_inventory(actor);
        ItemAt at{};
        if (!mgr || !find_in_bags(mgr, s.uid, at, equip_bags(!kliff), s.key)) {
            LOG("  step %zu: (companion) put on equipment-%u: key %u uid %llX is not in the bags; skipped", number,
                s.slot, s.key, s.uid);
        } else {
            // (Kliff from storage / the wardrobe: the request answers "Item not found" - the direct call, as before)
            bool direct = kliff && inv && (at.bag == kBagStorage || at.bag == kBagWardrobe);
            int err = direct ? call_equip(inv, at.bag, at.pos, s.slot) : call_equip_request(actor, at.item, at.pos, s.slot, at.bag);
            if (direct) LOG("  (from bag %u: the direct put-on)", at.bag);
            g_steps_ok += err == 0; // (a change went through: a model swap follows)
            g_step_err = err;
            LOG("  step %zu: (companion) put on equipment-%u: key %u \"%s\" uid %llX from bag %u pos %d: %s", number,
                s.slot, s.key, item_name(s.key).c_str(), s.uid, at.bag, at.pos, error_text(err).c_str());
        }
    } else if (!s.equip) {
        int err = call_unequip(inv, s.slot);
        g_steps_ok += err == 0; // (a change went through: a model swap follows)
        LOG("  step %zu: take off equipment-%u (key %u uid %llX): %s", number, s.slot, s.key, s.uid,
            error_text(err).c_str());
        bool learned = false;
        if (err == kErrNoItemAfterRemoval) {
            std::lock_guard<std::mutex> keys(g_keys_mu);
            learned = g_keep_slots.insert(s.slot).second;
        }
        if (learned) {
            LOG("  (equipment-%u can't be left empty: outfits that leave it empty match whatever is in it)", s.slot);
            save_outfits();
        }
    } else {
        static bool bags_logged = false;
        U64 mgr = call_inventory(actor);
        if (mgr && !bags_logged) {
            bags_logged = true;
            log_bags(mgr);
        }
        ItemAt at{};
        if (!mgr || !find_in_bags(mgr, s.uid, at, equip_bags(false), s.key)) {
            LOG("  step %zu: put on equipment-%u: key %u uid %llX is not in the bags (inventory %llX); skipped",
                number, s.slot, s.key, s.uid, mgr);
        } else {
            int err = call_equip(inv, at.bag, at.pos, s.slot);
            g_steps_ok += err == 0; // (a change went through: a model swap follows)
            g_step_err = err;
            LOG("  step %zu: put on equipment-%u: key %u \"%s\" uid %llX from bag %u pos %d: %s", number, s.slot,
                s.key, item_name(s.key).c_str(), s.uid, at.bag, at.pos, error_text(err).c_str());
        }
    }
    if (ctx[0]) release_player(ctx);
    return ok;
}

// Compares the worn armor with the outfit after loading it (slots left as they are don't count). Logs the result when
// everything matches or `final`; true if everything matches.
bool verify_loading(U64 self, const Outfit& o, const std::vector<U16>& leave, bool final)
{
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) {
        if (final) LOG("load \"%s\": done (not checked: %d)", o.name.c_str(), n);
        return final;
    }
    int match = 0, counted = 0;
    std::string differs;
    for (int i = 0; i < n; ++i) {
        if (std::find(leave.begin(), leave.end(), w[i].slot) != leave.end() || !loads_piece(o, w[i].slot)) continue;
        ++counted;
        const OutfitItem* t = outfit_piece(o, w[i].slot);
        bool ok = t ? w[i].key == t->key && w[i].uid == t->uid : w[i].key == 0xFFFF;
        ok |= hands_crossed(o, w, n, w[i].slot); // (its weapons in each other's hands: as worn)
        match += ok;
        if (!ok) {
            char b[128];
            std::snprintf(b, sizeof b, "\n  differs: equipment-%u wears key %u uid %llX, outfit key %u uid %llX",
                          w[i].slot, w[i].key, w[i].key != 0xFFFF ? w[i].uid : 0, t ? t->key : 0xFFFF, t ? t->uid : 0);
            differs += b;
        }
    }
    if (match == counted || final)
        LOG("load \"%s\": done, %d of %d slots match%s", o.name.c_str(), match, counted, differs.c_str());
    return match == counted;
}

// Asynchronous parts of a load: companions' requests apply a moment later, and a piece taken from another character
// lands in the bags only once their unequip went through. pump() finishes them on the first frame they can be: the
// taken piece is put on as soon as it is in a bag, and the load is done on the first frame the worn gear matches the
// outfit (it can't match early: until the changes apply, it differs). kAsyncMs is only the give-up for a piece the
// game refuses.
constexpr U64 kAsyncMs = 3000;

AsyncLoad g_async;

// "Cloak (Leather Cloak), Ring 1 (Silver Ring)": the outfit's pieces in these slots.
std::string slot_list(const Outfit& o, const std::vector<U16>& slots)
{
    std::string s;
    for (U16 slot : slots) {
        std::string one = slot_name(slot);
        for (const OutfitItem& it : o.items)
            if (it.slot == slot) {
                std::string n = item_name(it.key);
                if (!n.empty()) one = lang::f("piece.with_item", {{"slot", one}, {"item", n}});
                break;
            }
        s += (s.empty() ? std::string() : lang::f("list.separator")) + one;
    }
    return s;
}

// leave: the outfit's piece can't be put on there (a worn piece that isn't the outfit's still comes off);
// keep: untouched (a piece taken from another character is on its way there).

void load_outfit(U64 self, U32 index, const std::vector<U16>& leave = {}, const std::vector<U16>& keep = {})
{
    const Outfit& o = g_outfits[index];
    // (one outfit at a time: the last one is still going on the model; only while that load runs, so a part the game
    // leaves "loading" can never block loading for good)
    if (g_async.active && look_changing()) {
        LOG("load \"%s\": ignored, the last outfit is still being put on", o.name.c_str());
        report(self, lang::t("report.still_loading"));
        return;
    }
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) {
        LOG("load \"%s\": worn gear unreadable (%d)", o.name.c_str(), n);
        return;
    }
    look_hold(true); // (until the model swaps: the old pieces keep their looks, the new ones hide at once)
    mark_worn(static_cast<int>(index)); // (its looks: each piece as soon as it is on)
    std::vector<LoadStep> off, on;
    std::vector<LeavingPiece> leaving; // the worn pieces this load replaces or takes off (must still be owned after)
    std::set<U16> keep_slots; // (slots the game refuses to empty: asking only brings its error message and a wait)
    {
        std::lock_guard<std::mutex> keys(g_keys_mu);
        keep_slots = g_keep_slots;
    }
    U64 shared_mgr = 0; // (the inventory, for the shared-id check below)
    {
        U64 ctx[4] = {};
        U64 actor = acquire_player(ctx);
        shared_mgr = actor ? call_inventory(actor) : 0;
        if (ctx[0]) release_player(ctx);
    }
    std::string shared_names;
    for (int i = 0; i < n; ++i) {
        if (!loads_piece(o, w[i].slot)) continue; // (Weapons: Leave / saved before weapons)
        const OutfitItem* t = outfit_piece(o, w[i].slot);
        bool worn = w[i].key != 0xFFFF;
        bool left = std::find(leave.begin(), leave.end(), w[i].slot) != leave.end();
        bool kept = std::find(keep.begin(), keep.end(), w[i].slot) != keep.end();
        LOG("  slot %d (equipment-%u): worn key %u \"%s\" uid %llX at %u/%u, outfit key %u \"%s\" uid %llX%s", i,
            w[i].slot, w[i].key, worn ? item_name(w[i].key).c_str() : "", worn ? w[i].uid : 0, w[i].at[0], w[i].at[1],
            t ? t->key : 0xFFFF, t ? item_name(t->key).c_str() : "", t ? t->uid : 0,
            kept ? " (a taken piece comes here)" : left ? " (outfit piece unavailable)" : "");
        if (kept || (worn && t && w[i].uid == t->uid && w[i].key == t->key)) continue;
        if (hands_crossed(o, w, n, w[i].slot)) { // (its weapons, in each other's hands: left as they are)
            LOG("  equipment-%u: the outfit's weapon is in the other hand; left as it is", w[i].slot);
            continue;
        }
        // A worn piece whose id is also in a bag (two things share one id, #1): taking it off loses it, the game merges
        // it into the other entry. Its slot is left as it is.
        if (worn && shared_mgr) {
            std::string where;
            if (shared_id_in_bags(shared_mgr, w[i].uid, &where)) {
                LOG("  equipment-%u: worn \"%s\" uid %llX shares its id with: %s; left on (taking it off would lose it)",
                    w[i].slot, item_name(w[i].key).c_str(), w[i].uid, where.c_str());
                shared_names += (shared_names.empty() ? "" : ", ") + item_name(w[i].key);
                continue;
            }
        }
        if (left) {
            if (worn && !keep_slots.count(w[i].slot)) off.push_back({false, w[i].slot, w[i].key, w[i].uid}); // not the outfit's: off
            continue;
        }
        bool needed = false; // the worn piece goes to another slot of the outfit
        for (const OutfitItem& it : o.items) needed |= worn && it.uid == w[i].uid && it.key == w[i].key;
        // Every worn piece comes off before the outfit's goes on - no direct swaps (#1: with a stacking mod, pieces were
        // lost on direct swaps, never on a take-off first; a stacked outfit piece's swap put the worn one into the stack).
        const bool off_first = worn && t && !keep_slots.count(w[i].slot);
        if (worn && (needed || off_first || (!t && !keep_slots.count(w[i].slot))))
            off.push_back({false, w[i].slot, w[i].key, w[i].uid});
        if (t) on.push_back({true, t->slot, t->key, t->uid});
        if (worn && (t || !keep_slots.count(w[i].slot))) leaving.push_back({w[i].uid, w[i].key, w[i].slot}); // (checked after)
    }
    if (!shared_names.empty())
        notify(self, lang::f("toast.shared_left_on", {{"pieces", shared_names}}));
    if (off.empty() && on.empty() && !g_async.active) {
        LOG("load \"%s\": already worn", o.name.c_str());
        look_hold(false);
        look_refresh();
        return;
    }
    LOG("load \"%s\": %zu to take off, %zu to put on", o.name.c_str(), off.size(), on.size());
    off.insert(off.end(), on.begin(), on.end());
    std::vector<DeferredEquip> held;
    g_steps_ok = 0;
    std::set<U16> refused; // (slots whose take-off the game refused: nothing goes on there in this load)
    for (size_t i = 0; i < off.size(); ++i) {
        if (off[i].equip && refused.count(off[i].slot)) {
            LOG("  step %zu: put on equipment-%u skipped: its take-off was refused", i + 1, off[i].slot);
            continue;
        }
        if (!run_step(off[i], i + 1)) break;
        if (!off[i].equip && g_step_err) refused.insert(off[i].slot);
        if (!off[i].equip || !g_step_err || g_step_err == kFault) continue;
        U64 e = slot_claim(off[i].slot);
        if (!e) continue; // (refused for another reason: verify reports it)
        LOG("  equipment-%u is held by entry %llX (%s); put on once the game lets go of it", off[i].slot, e,
            hex(e, 16).c_str());
        held.push_back({off[i].uid, off[i].slot, off[i].key, slot_list(o, {off[i].slot}), "", true});
    }
    // The new pieces reach the inventory over the next frames (pump_async syncs each one; the model swaps after).
    // Nothing went through (every step refused): no swap is coming, so the looks apply in full.
    if (!g_steps_ok) look_hold(false);
    look_refresh();
    // Checked by pump once it settled: companions' requests apply a moment later, and even Kliff's direct changes
    // reach the worn list late when there are many (11 take-offs checked at once: "1 of 11").
    g_async = {o.name, leave, std::move(g_async.equips), GetTickCount64(), true};
    for (DeferredEquip& d : held) g_async.equips.push_back(std::move(d));
    g_async.leaving = std::move(leaving);
}

// "Helmet (Wooden Mask...)": one piece of the outfit.
std::string piece_label(const Outfit& o, U16 slot) { return slot_list(o, {slot}); }

// A piece's new uid, in every outfit of every character (the game gives an item a new uid when it moves to storage).
void follow_piece(U64 from, U64 to, U16 key)
{
    auto follow = [&](std::vector<Outfit>& list) {
        for (Outfit& o : list)
            for (OutfitItem& it : o.items)
                if (it.uid == from && it.key == key) it.uid = to;
    };
    follow(g_outfits);
    std::lock_guard<std::mutex> lock(g_store_mu);
    for (CharacterOutfits& ch : g_store)
        if (ch.character != g_character) follow(ch.outfits);
}

// Loads the outfit as the answers say: copies saved into it, pieces taken from other characters (unequipped from
// them now, put on by pump once they are in the bags), the rest of the left slots untouched; one notification.
void finish_load(U64 self, AskedLoad& p)
{
    if (p.index >= g_outfits.size()) return;
    Busy busy;
    Outfit& o = g_outfits[p.index];
    for (const auto& [k, uid] : p.copies) {
        if (k >= o.items.size()) continue;
        OutfitItem& it = o.items[k];
        if (!p.use_copies) {
            p.left.push_back({it.slot, "left.gone"});
            continue;
        }
        LOG("  equipment-%u: uid %llX -> copy %llX", it.slot, it.uid, uid);
        follow_piece(it.uid, uid, it.key);
    }
    if (p.use_copies && !p.copies.empty()) save_outfits();
    std::vector<U16> keep; // for load_outfit: untouched (a taken piece comes there)
    std::vector<DeferredEquip> equips;
    for (const Take& t : p.takes) {
        if (t.k >= o.items.size()) continue;
        const OutfitItem& it = o.items[t.k];
        if (!p.take) {
            p.left.push_back({it.slot, "left.stays_with", t.who});
            continue;
        }
        keep.push_back(it.slot);
        int err = call_unequip_request(t.actor, t.worn_slot);
        LOG("load \"%s\": taking equipment-%u key %u uid %llX from %s (their slot %u): %s", o.name.c_str(), it.slot,
            it.key, it.uid, character_name(t.who), t.worn_slot, error_text(err).c_str());
        equips.push_back({it.uid, it.slot, it.key, piece_label(o, it.slot), character_name(t.who)});
    }
    std::vector<U16> leave; // the outfit's piece can't come there
    for (const Left& l : p.left) leave.push_back(l.slot);
    g_async = {};
    if (!equips.empty()) g_async = {o.name, {}, std::move(equips), GetTickCount64(), true};
    load_outfit(self, p.index, leave, keep);
    if (g_async.active) {
        g_async.leave.clear();
        for (const Left& l : p.left) g_async.leave.push_back(l.slot); // (taken slots are checked)
    }
    if (!p.left.empty()) {
        std::string msg;
        for (const Left& l : p.left)
            msg += (msg.empty() ? std::string() : lang::f("list.separator2")) +
                   lang::f(l.why, {{"piece", piece_label(o, l.slot)}, {"who", character_name(l.who)}});
        report(self, lang::f("report.left", {{"outfit", o.name}, {"list", msg}}));
    }
    refresh_wheel(self);
}

// Space / click on a saved outfit: where is each piece? Then asks (pieces on other characters, copies of gone ones) or
// loads.
void start_loading(U64 self, U32 index)
{
    if (g_ask_modal || g_ask_next) return;
    if (fx_defer_load(self, index)) return; // (the outfit effect: the glow up first; fx_tick loads it at the peak)
    const Outfit& o = g_outfits[index];
    std::vector<Owned> owned;
    if (!read_owned(self, owned)) {
        LOG("load \"%s\": inventory unreadable; loading without the checks", o.name.c_str());
        load_outfit(self, index);
        return;
    }
    if (adopt_returned(g_outfits[index], owned)) save_outfits();
    AskedLoad p;
    p.index = index;
    p.name = o.name;
    bool main = playing_main_character();
    for (size_t k = 0; k < o.items.size(); ++k) {
        const OutfitItem& it = o.items[k];
        if (!loads_piece(o, it.slot)) continue; // (Weapons: Leave)
        const Owned* x = find_owned(owned, it.uid, it.key);
        if (!x) continue; // gone: copies / not in the inventory (below)
        const char* why = nullptr;
        switch (x->where) {
        case Where::Worn: break;
        case Where::Carried: break;
        case Where::Storage:
            if (!(equip_bags(!main) & bag_bit(kBagStorage))) why = "left.storage";
            break;
        case Where::Wardrobe:
            if (!(equip_bags(!main) & bag_bit(kBagWardrobe))) why = "left.wardrobe";
            break;
        case Where::Other: why = "left.gone"; break;
        case Where::Companion: p.takes.push_back({k, x->who, x->actor, x->slot}); break;
        }
        if (why) p.left.push_back({it.slot, why});
        LOG("load \"%s\": equipment-%u key %u uid %llX: %s%s%s", o.name.c_str(), it.slot, it.key, it.uid,
            x->where == Where::Worn ? "worn" : x->where == Where::Carried ? "carried" : x->where == Where::Storage ? "storage"
            : x->where == Where::Wardrobe ? "wardrobe" : x->where == Where::Other ? "other bag" : "worn by ",
            x->where == Where::Companion ? character_name(x->who) : "", why ? " (left as is)" : "");
    }
    PieceCheck c = check_pieces(o, owned);
    bool followed = false;
    for (const auto& [k, uid] : c.copies) {
        int same = 0; // (the only one owned: the same item under a new uid, nothing to ask)
        for (const Owned& x : owned) same += x.key == o.items[k].key;
        LOG("load \"%s\": equipment-%u key %u uid %llX is gone; copy %llX owned%s", o.name.c_str(), o.items[k].slot,
            o.items[k].key, o.items[k].uid, uid, same == 1 ? ": followed" : "");
        if (same == 1) follow_piece(o.items[k].uid, uid, o.items[k].key), followed = true;
        else p.copies.push_back({k, uid});
    }
    if (followed) save_outfits();
    for (size_t k : c.gone) {
        p.left.push_back({o.items[k].slot, "left.gone"});
        LOG("load \"%s\": equipment-%u key %u uid %llX is gone; no copy", o.name.c_str(), o.items[k].slot,
            o.items[k].key, o.items[k].uid);
    }
    // Most of the outfit can't be had (sold, or item data that no longer fits this save): loading would only strip the
    // character. A few gone pieces load as before: the rest of the outfit goes on, those slots are left.
    size_t pieces = 0;
    for (const OutfitItem& it : o.items) pieces += loads_piece(o, it.slot) && it.key != 0xFFFF;
    if (pieces && c.gone.size() * 2 > pieces) {
        LOG("load \"%s\": %zu of %zu pieces are gone; not loaded", o.name.c_str(), c.gone.size(), pieces);
        notify(self, lang::f("toast.not_loaded_missing", {{"outfit", o.name}, {"n", std::to_string(c.gone.size())},
                                                          {"total", std::to_string(pieces)}}));
        return;
    }
    if (!p.takes.empty() && !g_take_from_companions) { // rule: the outfit does not load; say who has the pieces
        std::string msg;
        for (const Take& t : p.takes)
            msg += (msg.empty() ? std::string() : lang::f("list.separator2")) +
                   lang::f("left.worn_by", {{"piece", piece_label(o, o.items[t.k].slot)}, {"who", character_name(t.who)}});
        LOG("load \"%s\": pieces on other characters and taking them is off; not loaded", o.name.c_str());
        notify(self, lang::f("toast.not_loaded", {{"outfit", o.name}, {"list", msg}}));
        return;
    }
    ask_next(self, std::move(p));
}
void lost_check(U64 self, const std::string& outfit, const std::vector<LeavingPiece>& leaving);

void pump_async(U64 self)
{
    pump_ask(self);
    if (!g_async.active) return;
    // (the new pieces reach the inventory a few frames after the requests, their model parts a little later: a look
    // set in between is queued on the still-loading part and lands with it, so nothing is drawn before it is hidden)
    look_refresh();
    U64 now = GetTickCount64();
    auto it = std::find_if(g_outfits.begin(), g_outfits.end(), [](const Outfit& o) { return o.name == g_async.outfit; });
    if (it == g_outfits.end()) {
        g_async = {};
        return;
    }
    bool late = now - g_async.started > kAsyncMs;
    bool acted = false; // a late put-on ran this frame: checked once the game applied it (a later frame)
    for (size_t i = 0; i < g_async.equips.size();) {
        DeferredEquip& d = g_async.equips[i];
        if (d.held) { // (the slot's holder is the game's state, read every frame: on as soon as it is free)
            if (slot_claim(d.slot) && !late) {
                ++i;
                continue;
            }
            if (!late) run_step({true, d.slot, d.key, d.uid}, 0), acted = true;
            if (late || g_step_err) {
                LOG("load \"%s\": %s still refused (%s)", g_async.outfit.c_str(), d.what.c_str(),
                    late ? "its slot stayed held" : error_text(g_step_err).c_str());
                report(self, lang::f("report.not_put_on", {{"outfit", g_async.outfit}, {"piece", d.what}}));
            } else {
                LOG("load \"%s\": %s went on once its slot was free", g_async.outfit.c_str(), d.what.c_str());
            }
            g_async.started = now;
            g_async.equips.erase(g_async.equips.begin() + i);
            continue;
        }
        U64 ctx[4] = {};
        U64 actor = acquire_player(ctx);
        U64 mgr = actor ? call_inventory(actor) : 0;
        ItemAt at{};
        bool there = mgr && find_in_bags(mgr, d.uid, at, bag_bit(kBagCarried) | bag_bit(kBagKeyItems), d.key);
        if (ctx[0]) release_player(ctx);
        if (there) {
            LOG("load \"%s\": %s from %s is in the bags now", g_async.outfit.c_str(), d.what.c_str(), d.from.c_str());
            run_step({true, d.slot, d.key, d.uid}, 0);
            acted = true;
            g_async.started = now; // (the check gets its own window: a late arrival must not be checked at once)
            g_async.equips.erase(g_async.equips.begin() + i);
        } else if (late) {
            LOG("load \"%s\": %s did not arrive from %s", g_async.outfit.c_str(), d.what.c_str(), d.from.c_str());
            report(self, lang::f("report.not_taken", {{"outfit", g_async.outfit}, {"piece", d.what}, {"who", d.from}}));
            g_async.equips.erase(g_async.equips.begin() + i);
        } else {
            ++i;
        }
    }
    if (!g_async.equips.empty() || acted) return;
    if (verify_loading(self, *it, g_async.leave, late) || late) {
        lost_check(self, g_async.outfit, g_async.leaving);
        g_async = {};
        look_settled();
        stash_pieces(self, &*it);
    }
}

// After a load: every piece it took off or replaced must still be somewhere (worn, a bag, a companion). One that is
// nowhere was lost by the game on the way to the bag (2026-11-06, a player's log: a piece swapped out by a load
// vanished; it couldn't be spawned again and came back after a restart, so the game still had it, in no bag). Named
// in the log, and the player is told: restarting without saving brings it back.
void lost_check(U64 self, const std::string& outfit, const std::vector<LeavingPiece>& leaving)
{
    if (leaving.empty()) return;
    std::vector<Owned> owned;
    if (!read_owned(self, owned)) {
        LOG("load \"%s\": loss check skipped (inventory unreadable)", outfit.c_str());
        return;
    }
    int lost = 0;
    for (const LeavingPiece& p : leaving) {
        bool found = false;
        for (const Owned& x : owned) found |= x.uid == p.uid && x.key == p.key;
        if (found) continue;
        ++lost;
        LOG("load \"%s\": LOST: key %u \"%s\" uid %llX left equipment-%u and is in no bag; the game still holds it "
            "(restart without saving)", outfit.c_str(), p.key, item_name(p.key).c_str(), p.uid, p.slot);
        notify(self, lang::f("toast.piece_lost", {{"piece", item_name(p.key)}}));
    }
    LOG("load \"%s\": loss check: %zu piece(s) left their slots, %d lost", outfit.c_str(), leaving.size(), lost);
}

// Before the outfits are shown or loaded (the wheel opening, the quick peek opening): a save just loaded is taken over,
// the character's own outfits are the ones in use, and their keys are checked against the permanent ids (a save
// written before a game patch brings back the old keys: a load before this repair fetched the wrong items, 2026-11-05).

void prepare_outfits()
{
    ensure_resist_ids();
    settle_load_now();
    pull_active();
    if (U16 ch = current_character(); ch != kNoCharacter) switch_character(ch);
    repair_keys();
}

} // namespace om
