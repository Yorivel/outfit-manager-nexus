// Saving, overwriting, deleting, cleaning up and uninstalling outfits.
#include "om.h"
#include "actions.h"
#include "addrs.h"
#include "ui.h"
#include "store.h"
#include "prompts.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "setup.h"
#include "om_rest.h"

namespace om {

std::string default_name()
{
    for (U32 n = 1;; ++n) {
        std::string s = lang::f("outfit.default_name", {{"n", std::to_string(n)}});
        bool used = false;
        for (const Outfit& o : g_outfits) used |= o.name == s;
        if (!used) return s;
    }
}

// The "+" slot pressed: save the worn armor as a new outfit, unless an identical one exists (then names it).
void save_new_outfit(U64 self)
{
    if (g_uninstalled) {
        notify(self, lang::t("toast.uninstalled"));
        return;
    }
    if (g_outfits.size() >= kMaxOutfits) {
        notify(self, lang::f("toast.limit", {{"n", std::to_string(kMaxOutfits)}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (const Outfit& s : g_outfits)
        if (same_saved(s, o)) {
            notify(self, lang::f("toast.already_saved", {{"outfit", s.name}}));
            return;
        }
    o.name = default_name();
    if (int a = anchor_outfit(); a >= 0) carry_looks(g_outfits[size_t(a)], o); // (a changed outfit saved as new looks the same)
    g_outfits.push_back(o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    LOG("saved \"%s\" (%zu items, chest %s)", o.name.c_str(), o.items.size(),
        o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
    open_rename_prompt(self, g_outfits.size() - 1);
}

// Delete (the outfit page, confirmed): the later outfits move up and the wheel shrinks.
void delete_outfit(U64 self, U32 o)
{
    LOG("deleted \"%s\"", g_outfits[o].name.c_str());
    g_outfits.erase(g_outfits.begin() + o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
}

// Clean up: this save's outfits, i.e. the current outfits of every character and the copies of the save loaded last
// (so loading it again does not bring them back; the other saves keep theirs, as with any game state).
void clean_outfits(U64 self)
{
    size_t gone = 0, saves = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        if (g_generation.load() != g_seen_generation) {
            LOG("clean up dropped: a save was loaded meanwhile");
            return;
        }
        store_for(g_character).outfits = g_outfits;
        gone = outfit_total(g_store);
        g_store.clear();
        g_published.clear();
        for (auto& [slot, b] : g_bound)
            if (g_loaded_known && b.id.same(g_loaded_id)) {
                if (!b.store.empty()) ++saves;
                b.store.clear();
            }
        bool ok = write_store_file();
        LOG("clean up: %zu current outfits and the copies of %zu saves deleted%s", gone, saves,
            ok ? "" : " (writing FAILED)");
    }
    g_outfits.clear();
    g_outfit_count = 0;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t("toast.cleared"));
}

// Uninstall: every file the mod keeps (the outfits of every save and the settings, the address cache, the log, a
// write's leftover) is deleted, so removing the mod in DMM leaves nothing behind. The mod never writes into game saves.
// It stays loaded until the game closes, inert: nothing it does writes a file again.
bool g_uninstalled = false;

void uninstall(U64 self)
{
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        g_uninstalled = true;
        g_store.clear(), g_published.clear(), g_bound.clear(), g_pending_writes.clear(), g_unfinished.clear();
    }
    g_outfits.clear();
    g_outfit_count = 0;
    LOG("uninstall: deleting the outfit file, the cache and this log; the mod stays inert until the game closes");
    logx::close();
    int failed = 0;
    for (const std::wstring& f : {g_store_path, g_store_path + L".tmp", g_cache_path, g_log_path})
        if (!f.empty() && !DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ++failed;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t(failed ? "toast.remove_failed" : "toast.removed"));
}

// The Clean up and Uninstall questions: a deleting button and Cancel (the dialog's second button is Esc's: Esc always
// cancels).
void open_clean_confirm(U64 self, int what)
{
    if (g_ask_modal || g_ask_next) return;
    std::string msg, yes;
    if (what == kConfirmCleanSave) {
        size_t total = 0;
        std::string who;
        store_for(g_character); // (the current character counts from its working list, g_outfits)
        for (const CharacterOutfits& c : g_store) {
            size_t n = c.character == g_character ? g_outfits.size() : c.outfits.size();
            if (!n) continue;
            total += n;
            who += (who.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.clear_who", {{"character", c.character == kNoCharacter ? lang::t("character.unassigned")
                                                                                        : character_name(c.character)},
                                             {"n", std::to_string(n)}});
        }
        if (!total) {
            notify(self, lang::t("toast.nothing_to_clear"));
            return;
        }
        msg = lang::f("ask.clear", {{"n", std::to_string(total)}, {"who", who}});
        yes = lang::t("ask.yes.delete");
    } else {
        msg = lang::t("ask.uninstall");
        yes = lang::t("ask.yes.remove");
    }
    const char* title = lang::t(what == kConfirmCleanSave ? "setting.clear" : "setting.uninstall");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, title, msg.c_str(), yes.c_str(), lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("clean up: the dialog did not open; nothing deleted");
        return;
    }
    AskedLoad p;
    p.confirm = what;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("clean up question %d %llX open: %s", what, modal, msg.c_str());
}

// Overwrite / Delete / Swap ask first, in the game's yes / no dialog (the loading questions' machinery: finish_ask
// answers). other: kConfirmSwap's second outfit; kConfirmOverwrite: 1 resets the outfit's settings, 0 keeps them.
void open_confirm(U64 self, U32 index, int what, U32 other)
{
    if (g_ask_modal || g_ask_next || index >= g_outfits.size()) return;
    if (what == kConfirmSwap && other >= g_outfits.size()) return;
    const std::string& name = g_outfits[index].name;
    std::string msg = what == kConfirmOverwrite ? lang::f(other ? "ask.overwrite.reset" : "ask.overwrite.keep", {{"outfit", name}})
                      : what == kConfirmDelete  ? lang::f("ask.delete", {{"outfit", name}})
                                                : lang::f("ask.swap", {{"outfit", name}, {"worn", g_outfits[other].name}});
    const char* yes = lang::t(what == kConfirmOverwrite ? "ask.yes.overwrite" : what == kConfirmDelete ? "ask.yes.delete" : "ask.yes.swap");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, name.c_str(), msg.c_str(), yes, lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("confirm %d for \"%s\": the dialog did not open; nothing done", what, name.c_str());
        return;
    }
    AskedLoad p;
    p.index = index, p.name = name, p.confirm = what;
    p.other = other; // (Swap: the other outfit; Overwrite: reset or keep)
    if (what == kConfirmSwap) p.other_name = g_outfits[other].name;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("confirm %d %llX open: %s", what, modal, msg.c_str());
}

void confirmed(U64 self, U32 index, int what, U32 other)
{
    if (index >= g_outfits.size()) return;
    if (what == kConfirmDelete) {
        delete_outfit(self, index);
        return;
    }
    if (what == kConfirmSwap) {
        if (other >= g_outfits.size() || other == index) return;
        std::swap(g_outfits[index], g_outfits[other]);
        LOG("Swap: \"%s\" and \"%s\" changed slots", g_outfits[other].name.c_str(), g_outfits[index].name.c_str());
        save_outfits();
        refresh_wheel(self);
        center_restore();
        center_for_focus(self, focused_script(self), true);
        report(self, lang::f("report.swapped", {{"a", g_outfits[other].name}, {"b", g_outfits[index].name}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            notify(self, lang::f(j == index ? "toast.already_holds" : "toast.already_saved", {{"outfit", g_outfits[j].name}}));
            return;
        }
    const Outfit& was = g_outfits[index];
    o.name = was.name;
    o.worn = was.worn; // (whether it is the last loaded one: not a setting)
    bool keep = other == 0;
    if (keep) { // (its settings and looks stay; reset: as newly saved)
        if (was.has_weapons) o.load_weapons = was.load_weapons;
        o.climate = was.climate, o.stash = was.stash;
        o.hk_vk = was.hk_vk, o.hk_mods = was.hk_mods;
        carry_looks(was, o);
    }
    g_outfits[index] = o;
    LOG("Overwrite (%s): \"%s\" now holds %zu items (chest %s)", keep ? "settings kept" : "settings reset",
        o.name.c_str(), o.items.size(), o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    report(self, lang::f("report.now_holds", {{"outfit", o.name}}));
}

// Overwrite: what is worn decides the question. Already this outfit: said so; another saved outfit: offer to swap
// their slots; anything else: confirm the overwrite.
void overwrite_pressed(U64 self, U32 index, bool reset)
{
    Outfit o;
    if (index >= g_outfits.size() || !snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            if (j == index) notify(self, lang::f("toast.already_holds", {{"outfit", g_outfits[j].name}}));
            else open_confirm(self, index, kConfirmSwap, static_cast<U32>(j));
            return;
        }
    open_confirm(self, index, kConfirmOverwrite, reset ? 1 : 0);
}

} // namespace om
