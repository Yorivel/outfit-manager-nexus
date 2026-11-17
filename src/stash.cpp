// Stash: saved pieces left in the bag after a load go to storage.
#include "om.h"
#include "stash.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "store.h"
#include "load.h"
#include "settings.h"

namespace om {

// --- Stash: after a load, every saved piece (any outfit, any character) left in the carried bag goes to storage,
// through the storage screen's own move (as when the player moves it there; works anywhere). The piece gets a new uid
// there; stash_verify follows it exactly. Key items (bag 4) stay: the game keeps them on you.
int stash_rule(U16 from, U16 to) // the source bag's transfer rule to `to` (-1: none, e.g. no wardrobe yet)
{
    __try {
        U64 info = by_key(g_bag_info, from);
        U64 rules = info ? *reinterpret_cast<U64*>(info + 0x38) : 0;
        U32 n = info ? *reinterpret_cast<U32*>(info + 0x40) : 0;
        for (U32 i = 0; rules && i < n && i < 256; ++i) {
            U64 r = rules + i * 0xa0ull;
            if (*reinterpret_cast<char*>(r) != 3 && *reinterpret_cast<U16*>(r + 2) == from &&
                *reinterpret_cast<U16*>(r + 4) == to)
                return static_cast<int>(i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

int call_item_move(U64 mgr, U32 owner, const ItemAt& at, U32 rule)
{
    int err = kFault;
    __try {
        short key = *reinterpret_cast<short*>(at.item + 8);
        U16 extra = *reinterpret_cast<U16*>(at.item + 0xa);
        g_item_move(mgr, &err, owner, owner, key, extra, 1, at.bag, static_cast<U16>(at.pos), rule);
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}
StashCheck g_stash_check;
constexpr U64 kStashCheckMs = 1500; // (only the give-up: a move counts when its piece is found in storage)

bool stash_pending(U64 uid)
{
    if (!g_stash_check.at) return false;
    for (const StashSent& s : g_stash_check.sent)
        if (s.uid == uid) return true;
    return false;
}

std::string pieces_text(int n) { return lang::n("count.pieces", n); }

// Every frame after the requests (from the HUD's frame too, so also with the wheel closed) until each piece is in
// storage (a few frames). A piece still in the bag at the give-up was refused (the game says why itself).
void stash_verify(U64 self)
{
    if (!g_stash_check.at) return;
    bool late = GetTickCount64() - g_stash_check.at >= kStashCheckMs;
    const StashCheck& c = g_stash_check;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    std::vector<Owned> now(4096, Owned{0, 0xFFFF, Where::Other});
    int n = mgr ? read_bags_raw(mgr, now.data(), static_cast<int>(now.size())) : -1;
    now.resize(n > 0 ? n : 0);
    if (ctx[0]) release_player(ctx);
    int moved = 0, left = 0, pending = 0;
    std::vector<std::pair<U64, U64>> renamed; // old uid -> new uid
    std::vector<U64> claimed;
    for (const StashSent& s : c.sent) {
        bool found = false;
        for (const Owned& x : now)
            if (x.key == s.key && x.where == Where::Storage && x.uid != s.uid &&
                std::find(c.before.begin(), c.before.end(), x.uid) == c.before.end() &&
                std::find(claimed.begin(), claimed.end(), x.uid) == claimed.end()) {
                claimed.push_back(x.uid);
                renamed.push_back({s.uid, x.uid});
                found = true;
                break;
            }
        if (found) ++moved;
        else if (late) ++left;
        else ++pending;
    }
    if (pending) return; // (still on the way: next frame)
    g_stash_check.at = 0; // (done: stash_pending is off from here)
    if (!mgr) return;
    if (!renamed.empty()) { // every outfit using the piece, every character
        int uses = 0;
        auto follow = [&](std::vector<Outfit>& list) {
            for (Outfit& o : list)
                for (OutfitItem& it : o.items)
                    for (const auto& [from, to] : renamed)
                        if (it.uid == from) it.uid = to, ++uses;
        };
        follow(g_outfits);
        {
            std::lock_guard<std::mutex> lock(g_store_mu);
            for (CharacterOutfits& ch : g_store)
                if (ch.character != g_character) follow(ch.outfits);
        }
        for (const auto& [from, to] : renamed) LOG("stash: uid %llX is now %llX", from, to);
        LOG("stash: %zu pieces followed (%d outfit slots updated)", renamed.size(), uses);
        save_outfits();
    }
    LOG("stash: %d put in storage, %d not", moved, left);
    if (moved && left) report(self, lang::f("report.stash.moved_left", {{"moved", pieces_text(moved)}, {"left", pieces_text(left)}}));
    else if (moved) report(self, lang::f("report.stash.moved", {{"moved", pieces_text(moved)}}));
    else if (left) report(self, lang::f("report.stash.left", {{"left", pieces_text(left)}}));
}
std::vector<StashDest> stash_destinations()
{
    std::vector<StashDest> out;
    auto add = [&](const std::vector<Outfit>& list) {
        for (const Outfit& o : list)
            for (const OutfitItem& it : o.items) {
                if (!loads_piece(o, it.slot)) continue; // (a weapon it leaves: not its business)
                bool on = stash_of(o);
                auto f = std::find_if(out.begin(), out.end(),
                                      [&](const StashDest& e) { return e.uid == it.uid && e.key == it.key; });
                if (f == out.end()) out.push_back({it.uid, it.key, on});
                else f->on = f->on && on;
            }
    };
    add(g_outfits);
    std::lock_guard<std::mutex> lock(g_store_mu);
    for (const CharacterOutfits& c : g_store)
        if (c.character != g_character) add(c.outfits);
    return out;
}

// After a load: saved pieces left in the carried bag go to storage, through the storage screen's own move (it works
// anywhere). Loading takes pieces from the bag, storage and the wardrobe alike. (The wardrobe was tried and dropped: away
// from the house the game counts only its base 10 slots, checked twice, once on its own side - 2026-10-27 tests.)
void stash_pieces(U64 self, const Outfit* loaded)
{
    if (!g_item_move || !g_bag_info) return;
    std::vector<StashDest> dest = stash_destinations();
    if (loaded) // (the outfit just loaded: its pieces are meant to be worn, even one still on its way to its slot)
        for (StashDest& d : dest)
            for (const OutfitItem& it : loaded->items)
                if (it.uid == d.uid && it.key == d.key) d.on = false;
    if (std::none_of(dest.begin(), dest.end(), [](const StashDest& e) { return e.on; })) return;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    U32 owner = actor ? r32(actor + 0x60) : 0; // (the storage screen passes the controlled character for both bags)
    int rule = mgr ? stash_rule(kBagCarried, kBagStorage) : -1;
    int failed = 0, err = 0;
    StashCheck check;
    {
        std::vector<Owned> held(4096, Owned{0, 0xFFFF, Where::Other});
        int n = mgr ? read_bags_raw(mgr, held.data(), static_cast<int>(held.size())) : -1;
        for (int i = 0; i < n; ++i)
            if (held[i].where == Where::Storage) check.before.push_back(held[i].uid);
    }
    for (const auto& [uid, key, on] : dest) {
        ItemAt at{};
        if (!on || rule < 0 || !find_in_bags(mgr, uid, at, bag_bit(kBagCarried), key)) continue;
        int e = call_item_move(mgr, owner, at, static_cast<U32>(rule));
        LOG("stash: uid %llX (key %u \"%s\") bag %u pos %d -> storage (rule %d, owner %08X): %s", uid, key,
            item_name(key).c_str(), at.bag, at.pos, rule, owner, error_text(e).c_str());
        if (!e) check.sent.push_back({uid, key});
        else ++failed, err = e;
    }
    if (ctx[0]) release_player(ctx);
    if (rule < 0 && mgr) {
        LOG("stash: no transfer rule from the bag to storage");
        return;
    }
    if (failed) // (refused at once; the ones sent are checked by stash_verify)
        report(self, lang::f("report.stash.failed", {{"pieces", pieces_text(failed)}, {"error", error_text(err)}}));
    if (!check.sent.empty()) {
        check.at = GetTickCount64();
        g_stash_check = std::move(check);
    }
}

} // namespace om
