// Self-test.
#include "om.h"
#include "selftest.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "looks.h"
#include "load.h"
#include "wheel_hooks.h"

namespace om {

void self_test(U64 self)
{
    static bool model_done = false, bags_done = false, names_done = false;
    ensure_resist_ids();
    if (U64 tb = rp(self + kOffRootTabBar); tb && !g_tab_checked) { // (the current tab: only ever one of the headers)
        U32 cur = r32(tb + kOffTabCurrent), headers = r32(tb + kOffTabHeaderCount);
        g_tab_checked = true;
        LOG("self-test: current tab %u of %u headers: %s", cur, headers, cur < headers ? "ok" : "FAILED");
        if (cur >= headers && g_layout_bad.empty()) g_layout_bad = "tab state";
    }
    if (model_done && bags_done && names_done) return;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    if (actor && !model_done) { // the character model behind the actor and its part table: Visibility
        U64 model = look_model(actor);
        U64 tab = model ? rp(model + 0x58) : 0;
        U32 n = model ? r32(model + 0x60) : 0;
        if (model) {
            U32 named = 0;
            for (U32 i = 0; tab && i < n && i < 1024; ++i)
                if (U64 part = rp(tab + i * 16ull + 8); part && readable_name(part_name(r32(part + 0x48)))) ++named;
            bool ok = tab && n >= 4 && n <= 1024 && named * 2 >= n;
            LOG("self-test: character model %llX: %u parts, %u named: %s", model, n, named, ok ? "ok" : "FAILED");
            if (!ok && g_vis_set) {
                g_vis_set = nullptr;
                feature_off("feature.visibility_rules");
            }
            model_done = true;
        }
    }
    if (actor && !bags_done) { // the bags and their capacity: Stash
        if (U64 mgr = call_inventory(actor)) {
            U64 bags = rp(mgr + kOffMgrBags);
            U32 n = r32(mgr + 0x20), sized = 0, odd = 0;
            for (U32 i = 0; bags && i < n && i < 64; ++i) {
                U64 bag = rp(bags + i * 8ull);
                short cap = 0;
                if (!bag || !rd(bag + kOffBagCount, cap)) continue;
                if (cap > 0 && cap <= 5000) ++sized;
                else if (cap != 0) ++odd;
            }
            bool ok = n >= 1 && n <= 64 && sized >= 1 && !odd;
            LOG("self-test: %u bags, %u with a capacity, %u odd: %s", n, sized, odd, ok ? "ok" : "FAILED");
            if (!ok && g_item_move) {
                g_item_move = nullptr;
                feature_off("feature.stash");
            }
            bags_done = true;
        }
    }
    if (ctx[0]) release_player(ctx);
    if (!names_done && g_item_info) { // a worn piece's key and name (the item record's layout): the names shown, only
        WornRaw w[kWornMax];
        int n = read_worn(self, w, kWornMax), worn = 0, named = 0;
        for (int i = 0; i < n; ++i)
            if (w[i].key != 0xFFFF) ++worn, named += readable_name(item_name(w[i].key));
        if (worn) {
            LOG("self-test: %d worn pieces, %d named: %s", worn, named, named ? "ok" : "FAILED");
            if (!named) feature_off("feature.item_names");
            names_done = true;
        }
    }
}

} // namespace om
