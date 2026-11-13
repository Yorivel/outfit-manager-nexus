// The outfits file (OutfitManager.outfits): reading, writing, one list per character.
#include "om.h"
#include "store.h"
#include "items.h"
#include "gear.h"
#include "looks.h"
#include "prompts.h"
#include "actions.h"
#include "settings.h"
#include "wheel_draw.h"
#include "arc.h"

namespace om {

// --- Outfit store -------------------------------------------------------------------------------
// OutfitManager.outfits: "character\t<key>" starts a character's list (outfits before the first one have
// no character), then one record per outfit: "outfit\t<name>", optional "chest\t<icon>", then
// "item\t<equip slot>\t<item key>\t<uid hex>" lines.
// That first part is the active set (the loaded game's outfits). Then one part per game save slot the outfits are
// bound to: "save\t<slot>\t<save.save size>\t<write time hex>\t<content hash hex>", followed by lists as above.
// Last, saves the game was still writing: "pending\t<slot>\t<start FILETIME hex>", followed by lists as above.
CharacterOutfits& list_for(Store& s, U16 character)
{
    for (CharacterOutfits& c : s)
        if (c.character == character) return c;
    s.push_back({character, {}});
    return s.back();
}

CharacterOutfits& store_for(U16 character) { return list_for(g_store, character); }

size_t outfit_total(const Store& s)
{
    size_t n = 0;
    for (const CharacterOutfits& c : s) n += c.outfits.size();
    return n;
}
std::mutex g_store_mu;
Store g_published;
std::map<U32, BoundSet> g_bound; // per game save slot: the save the mod last saw there + its outfits
SaveId g_loaded_id;               // the save loaded last (its copies are "this save"'s for Clean up)
bool g_loaded_known = false;
std::vector<PendingWrite> g_pending_writes;
std::vector<PendingWrite> g_unfinished; // pending writes read from the file, bound once the save folder is known
std::atomic<U32> g_generation{0};
U32 g_seen_generation = 0; // UI thread

// The outfit file: "# ..." comment, "version\t<n>", the settings, the lists; last "end\t<lines before it>\t<FNV-1a 64 of
// every byte before it>". A file whose end line does not match was damaged outside the plugin: it is left as it is and
// the session runs read-only. Files without one (older builds) are read as before; the next write adds it.
constexpr int kStoreVersion = 1;

void put(std::string& out, const char* fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = std::vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    if (n > 0) {
        size_t at = out.size();
        out.resize(at + size_t(n) + 1);
        std::vsnprintf(&out[at], size_t(n) + 1, fmt, b);
        out.resize(at + size_t(n));
    }
    va_end(b);
}

U64 fnv1a(const char* p, size_t n)
{
    U64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 0x100000001b3ull;
    return h;
}

void write_lists(std::string& out, const Store& s)
{
    for (int pass = 0; pass < 2; ++pass) // the character-less list first (it has no header)
        for (const CharacterOutfits& c : s) {
            if ((c.character == kNoCharacter) != (pass == 0) || (c.outfits.empty() && !c.helmet_pin && !c.no_outfit_set()))
                continue;
            if (pass) put(out, "character\t%u\n", c.character);
            if (pass && c.helmet_pin) put(out, "helmetpin\t%u\n", c.helmet_pin);
            if (pass) { // (#33: the No Outfit looks; slot 23 the back shield, 30 the underwear, as the looks publish them)
                const Outfit& n = c.no_outfit;
                for (const auto& [slot, look] : n.empty_looks)
                    if (look) put(out, "nolook\t%u\t%u\n", slot, look);
                if (n.back_look) put(out, "nolook\t%u\t%u\n", kBackShieldSlot, n.back_look);
                if (n.underwear_look) put(out, "nolook\t%u\t%u\n", kUnderwearSlot, n.underwear_look);
            }
            for (const Outfit& o : c.outfits) {
                put(out, "outfit\t%s\n", o.name.c_str());
                if (!o.chest_icon.empty()) put(out, "chest\t%s\n", o.chest_icon.c_str());
                if (o.seen) put(out, "seen\t%llX\n", o.seen);
                // (2 = Weapons only, #27: an older version reads it as Load)
                if (o.has_weapons) put(out, "weapons\t%d\n", o.weapons_only ? 2 : o.load_weapons ? 1 : 0);
                if (!o.weapon_icon.empty()) put(out, "weaponicon\t%s\n", o.weapon_icon.c_str());
                if (o.climate) put(out, "climate\t%u\n", o.climate);
                if (o.stash) put(out, "stash\t%u\n", o.stash);
                if (o.activity) put(out, "activity\t%u\n", o.activity);
                if (o.hk_vk) put(out, "hotkey\t%u\t%u\n", o.hk_vk, o.hk_mods);
                if (o.worn) put(out, "worn\t1\n");
                if (o.pin >= 0) put(out, "pin\t%d\n", o.pin);
                if (o.used) put(out, "used\t%u\n", o.used);
                if (o.back_look) put(out, "backlook\t%u\n", o.back_look);
                if (o.underwear_look) put(out, "underwear\t%u\n", o.underwear_look);
                if (o.has_back) put(out, "backshield\t%u\n", o.has_back);
                for (const auto& [slot, look] : o.empty_looks)
                    if (look) put(out, "slotlook\t%u\t%u\n", slot, look);
                for (const OutfitItem& it : o.items) {
                    if (it.pid) put(out, "item\t%u\t%u\t%llX\t%u\t%X\n", it.slot, it.key, it.uid, it.look, it.pid);
                    else if (it.look) put(out, "item\t%u\t%u\t%llX\t%u\n", it.slot, it.key, it.uid, it.look);
                    else put(out, "item\t%u\t%u\t%llX\n", it.slot, it.key, it.uid);
                }
            }
        }
}

// Set when the outfit file exists but could not be read at start-up (locked by a scanner / sync tool / editor):
// writing then would replace the user's outfits with an empty list, so nothing is written this session.
bool g_store_unreadable = false;
const char* g_store_why = ""; // why (for the message in game)

// Writes g_published + g_bound (g_store_mu held).

std::vector<U32> g_placeholder_ids;         // the file's, until the item table is there

bool write_store_file()
{
    if (g_store_path.empty() || g_uninstalled) return false; // (uninstalled: its file is gone and stays gone)
    if (g_store_unreadable) {
        LOG("outfit file not written: it could not be read at start-up (the outfits in it are kept as they are)");
        return false;
    }
    std::string out = "# Outfit Manager outfits\n";
    put(out, "version\t%d\n", kStoreVersion);
    {
        std::lock_guard<std::mutex> keys(g_keys_mu);
        if (!g_placeholder_keys.empty()) {
            std::string ph;
            std::string ids; // (by permanent id when all are known: a key would name another item after a renumbering)
            bool all = true;
            for (U16 k : g_placeholder_keys) {
                U32 id = pid_of(k);
                all = all && id;
                char one[16];
                std::snprintf(one, sizeof one, "%s%X", ids.empty() ? "" : " ", id);
                ids += one;
                ph += (ph.empty() ? "" : " ") + std::to_string(k);
            }
            if (all) put(out, "placeholderids\t%s\n", ids.c_str());
            else put(out, "placeholders\t%s\n", ph.c_str());
        }
        std::string ks;
        for (U16 k : g_keep_slots) ks += (ks.empty() ? "" : " ") + std::to_string(k);
        put(out, "keepslots\t%s\n", ks.c_str());
    }
    put(out, "setting\tstash\t%d\nsetting\tcompanions\t%d\nsetting\tmessages\t%d\n", g_stash ? 1 : 0,
        g_take_from_companions ? 1 : 0, g_messages ? 1 : 0);
    put(out, "setting\teffects\t%d\n", int(g_effects));
    if (!g_unmanaged.empty()) {
        std::string u;
        for (const auto& [c, o] : g_unmanaged) u += (u.empty() ? "" : " ") + std::to_string(c) + ":" + std::to_string(o);
        put(out, "setting\tunmanaged\t%s\n", u.c_str());
    }
    if (!g_language.empty()) put(out, "setting\tlanguage\t%s\n", g_language.c_str());
    if (const char* id = peek_key_id()) put(out, "setting\tpeekkey\t%s\n", id);
    if (g_peek_pad) put(out, "setting\tpeekpad\t1\n");
    if (U32 hv = g_helmet_vk.load()) put(out, "setting\thelmetkey\t%u\t%u\n", hv, unsigned(g_helmet_mods.load()));
    write_lists(out, g_published);
    for (const auto& [slot, b] : g_bound) {
        if (b.id.clock) // (+ its world clock and place, savekey.cpp; older builds read the first fields only)
            put(out, "save\t%u\t%llu\t%llX\t%llX\t%llX\t%X\n", slot, b.id.size, b.id.time, b.id.hash, b.id.clock, b.id.place);
        else
            put(out, "save\t%u\t%llu\t%llX\t%llX\n", slot, b.id.size, b.id.time, b.id.hash);
        write_lists(out, b.store);
    }
    for (const auto* list : {&g_pending_writes, &g_unfinished})
        for (const PendingWrite& p : *list) {
            put(out, "pending\t%u\t%llX\n", p.slot, p.started);
            write_lists(out, p.store);
        }
    size_t lines = static_cast<size_t>(std::count(out.begin(), out.end(), '\n'));
    put(out, "end\t%zu\t%016llX\n", lines, fnv1a(out.data(), out.size()));
    std::wstring tmp = g_store_path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        LOG("cannot write the outfit file");
        return false;
    }
    bool written = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    bool ok = std::fclose(f) == 0 && written && MoveFileExW(tmp.c_str(), g_store_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) LOG("writing the outfit file FAILED");
    return ok;
}

// Closes the rename prompt / missing-pieces question (nothing renamed or loaded). Their answers refer to an entry of
// the current outfit list, so they must not outlive it (save loaded, character changed, menu closed).

// UI thread: takes over the active set a game load put in place.
bool pull_active()
{
    if (g_generation.load() == g_seen_generation) return false;
    abort_prompts("a save was loaded"); // (before the lock: an answer may save)
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_seen_generation = g_generation.load();
    g_store = g_published;
    g_outfits = store_for(g_character).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    publish_looks(g_store);
    LOG("active outfits replaced by the loaded save's: %zu for character %u (%zu in all)", g_outfits.size(),
        g_character, outfit_total(g_store));
    return true;
}

// #32: outfits read from a file before pins were kept are pinned, after the pinned ones, in the wheel's order.
void normalize_pins(std::vector<Outfit>& v)
{
    int top = 0;
    for (const Outfit& o : v) top = std::max(top, o.pin);
    for (Outfit& o : v)
        if (o.pin < 0) o.pin = ++top;
}

void save_outfits()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    normalize_pins(g_outfits);
    if (g_generation.load() != g_seen_generation) {
        LOG("outfit change dropped: a save was loaded meanwhile");
        return;
    }
    { // (temporary outfits are never saved: sleep.cpp)
        std::vector<Outfit>& kept = store_for(g_character).outfits;
        kept.clear();
        for (const Outfit& o : g_outfits)
            if (!o.temp) kept.push_back(o);
    }
    g_published = g_store;
    publish_looks(g_store);
    bool ok = write_store_file();
    LOG("saved %zu outfits (%zu for character %u)%s", outfit_total(g_store), g_outfits.size(), g_character,
        ok ? "" : " (FAILED)");
}

// nullptr if the file text may be read: its end line matches (or it has none: an older build wrote it) and its version
// is not newer than this build's; otherwise what is wrong with it.
const char* check_store_text(const std::string& text)
{
    size_t endpos = text.rfind("\nend\t");
    if (endpos != std::string::npos) {
        size_t body = endpos + 1;
        unsigned long long lines = 0, hash = 0;
        if (sscanf_s(text.c_str() + body, "end\t%llu\t%llX", &lines, &hash) != 2) return "damaged (its end line)";
        size_t rest = text.find('\n', body);
        if (rest != std::string::npos && text.find_first_not_of("\r\n", rest) != std::string::npos)
            return "damaged (text after its end line)";
        if (size_t(std::count(text.begin(), text.begin() + body, '\n')) != lines) return "damaged (lines missing)";
        if (fnv1a(text.data(), body) != hash) return "damaged (its contents changed)";
    } else {
        LOG("the outfit file has no end check (written by an older build); read as before");
    }
    size_t v = text.find("\nversion\t");
    if (v != std::string::npos && std::atoi(text.c_str() + v + 9) > kStoreVersion) return "from a newer Outfit Manager";
    return nullptr;
}

void load_outfits()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_store.clear();
    g_bound.clear();
    g_unfinished.clear();
    std::map<U32, Store> legacy; // first binding build: slot-only entries ("save\t<slot>\t<size>\t<time>")
    FILE* f = nullptr;
    // A missing file is a fresh start; one that exists but can't be opened (briefly locked) gets a few retries, then
    // the session runs read-only (g_store_unreadable) instead of overwriting it.
    DWORD attr = GetFileAttributesW(g_store_path.c_str());
    DWORD gone = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    bool exists = attr != INVALID_FILE_ATTRIBUTES || (gone != ERROR_FILE_NOT_FOUND && gone != ERROR_PATH_NOT_FOUND);
    for (int attempt = 0; exists && attempt < 10 && (_wfopen_s(&f, g_store_path.c_str(), L"rb") != 0 || !f); ++attempt)
        Sleep(200);
    if (exists && !f) {
        g_store_unreadable = true, g_store_why = "locked (another program has it open)";
        LOG("the outfit file exists but cannot be read (error %lu); this session will not write it", GetLastError());
    }
    std::string text;
    if (f) {
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        f = nullptr;
        if (const char* why = check_store_text(text)) {
            g_store_unreadable = true, g_store_why = why;
            LOG("the outfit file is %s; it is left as it is and this session will not write it (edited by hand? delete its last "
                "line, \"end ...\", and it is read normally)", why);
            text.clear();
        }
    }
    if (!text.empty()) {
        Store* store = &g_store;
        CharacterOutfits* cur = &store_for(kNoCharacter);
        bool skipping = false;
        for (size_t at = 0; at < text.size();) {
            size_t nl = text.find('\n', at);
            std::string s = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            std::vector<std::string> v;
            for (size_t p = 0;;) {
                size_t t = s.find('\t', p);
                v.push_back(s.substr(p, t == std::string::npos ? std::string::npos : t - p));
                if (t == std::string::npos) break;
                p = t + 1;
            }
            // (an outfit beyond the wheel's slots is dropped with every line of it, not merged into the one before)
            if (v[0] == "outfit") {
                skipping = cur->outfits.size() >= kMaxOutfits;
                if (skipping) LOG("outfit file: \"%s\" is beyond the %u outfits a character can have; not read", v.size() >= 2 ? v[1].c_str() : "", kMaxOutfits);
                if (skipping) continue;
            } else if (v[0] == "character" || v[0] == "save" || v[0] == "pending") {
                skipping = false;
            } else if (skipping && v[0] != "setting" && v[0] != "keepslots" && v[0] != "placeholders" && v[0] != "placeholderids" && v[0] != "version") {
                continue;
            }
            std::vector<Outfit>& list = cur->outfits;
            if (v[0] == "save" && v.size() >= 4) {
                U32 slot = static_cast<U32>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (v.size() >= 5) {
                    BoundSet& b = g_bound[slot];
                    b.id = {std::strtoull(v[2].c_str(), nullptr, 10), std::strtoull(v[3].c_str(), nullptr, 16),
                            std::strtoull(v[4].c_str(), nullptr, 16)};
                    if (v.size() >= 7)
                        b.id.clock = std::strtoull(v[5].c_str(), nullptr, 16),
                        b.id.place = static_cast<U32>(std::strtoul(v[6].c_str(), nullptr, 16));
                    store = &b.store;
                } else {
                    store = &legacy[slot];
                }
                cur = &list_for(*store, kNoCharacter);
            } else if (v[0] == "pending" && v.size() >= 3) {
                g_unfinished.push_back({static_cast<U32>(std::strtoul(v[1].c_str(), nullptr, 10)), {}, {}, {}, 0,
                                        std::strtoull(v[2].c_str(), nullptr, 16)});
                store = &g_unfinished.back().store;
                cur = &list_for(*store, kNoCharacter);
            } else if (v[0] == "keepslots" && v.size() >= 2) { // (slots the game won't empty, learned)
                const char* q = v[1].c_str();
                char* e = nullptr;
                for (unsigned long k = std::strtoul(q, &e, 10); e != q; k = std::strtoul(q = e, &e, 10))
                    if (k < 0xFFFF) g_keep_slots.insert(U16(k));
            } else if (v[0] == "placeholderids" && v.size() >= 2) { // (turned into keys at the first menu open)
                std::istringstream ids(v[1]);
                for (std::string one; ids >> one;) g_placeholder_ids.push_back(static_cast<U32>(std::strtoul(one.c_str(), nullptr, 16)));
            } else if (v[0] == "placeholders" && v.size() >= 2) { // (the game's placeholder items, seen before)
                const char* p = v[1].c_str();
                char* e = nullptr;
                for (unsigned long k = std::strtoul(p, &e, 10); e != p; k = std::strtoul(p = e, &e, 10))
                    if (k < 0xFFFF && std::find(g_placeholder_keys.begin(), g_placeholder_keys.end(), U16(k)) ==
                                          g_placeholder_keys.end())
                        g_placeholder_keys.push_back(U16(k));
            } else if (v[0] == "setting" && v.size() >= 3) { // (the plugin's settings, set on the "+" slot's page)
                int x = std::atoi(v[2].c_str());
                // stash 0/1; older files: keep 0 Bag / 1 Storage / 2 Wardrobe, before it pull / stash (0 = off)
                if (v[1] == "stash" || v[1] == "keep" || v[1] == "pull") g_stash = x > 0;
                // (other names: settings of another build, e.g. the dropped "hotkey" / "wardrobe_bonus": ignored)
                else if (v[1] == "companions") g_take_from_companions = x != 0;
                else if (v[1] == "messages") g_messages = x != 0;
                else if (v[1] == "unmanaged") {
                    g_unmanaged.clear();
                    for (const char* q = v[2].c_str(); *q;) {
                        char* end = nullptr;
                        unsigned long c = std::strtoul(q, &end, 10);
                        if (end == q || *end != ':') break;
                        const char* o = end + 1;
                        long x = std::strtol(o, &end, 10);
                        if (end == o) break;
                        g_unmanaged[static_cast<U16>(c)] = int(x);
                        q = end;
                        while (*q == ' ') ++q;
                    }
                } else if (v[1] == "effects") g_effects = static_cast<std::uint8_t>(x < 0 ? 0 : x > 2 ? 2 : x);
                else if (v[1] == "language") g_language = v[2];
                else if (v[1] == "peekkey") peek_key_load(v[2]);
                else if (v[1] == "peekpad") g_peek_pad = x != 0;
                else if (v[1] == "helmetkey") {
                    g_helmet_vk = x > 0 && x < 256 ? U32(x) : 0;
                    g_helmet_mods = static_cast<std::uint8_t>(v.size() >= 4 ? std::strtoul(v[3].c_str(), nullptr, 10) & 15 : 0);
                }
                // (other names: settings of another build, e.g. the dropped "hotkey": ignored)
            } else if (v[0] == "character" && v.size() >= 2) {
                cur = &list_for(*store, static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10)));
            } else if (v[0] == "nolook" && v.size() >= 3) {
                const U16 slot = static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10));
                const unsigned long look = std::strtoul(v[2].c_str(), nullptr, 10);
                if (look > 0 && look < kLookCount) {
                    Outfit& n = cur->no_outfit;
                    if (slot == kBackShieldSlot) n.back_look = static_cast<std::uint8_t>(look);
                    else if (slot == kUnderwearSlot) n.underwear_look = static_cast<std::uint8_t>(look);
                    else if (slot < 32) n.empty_looks[slot] = static_cast<std::uint8_t>(look);
                }
            } else if (v[0] == "helmetpin" && v.size() >= 2) {
                const unsigned long p = std::strtoul(v[1].c_str(), nullptr, 10);
                cur->helmet_pin = p == 1 || p == 2 ? static_cast<std::uint8_t>(p) : 0;
            } else if (v[0] == "outfit" && v.size() >= 2) {
                list.push_back({v[1], {}, {}});
            } else if (!list.empty() && v[0] == "chest" && v.size() >= 2) {
                list.back().chest_icon = v[1];
            } else if (!list.empty() && v[0] == "seen" && v.size() >= 2) {
                list.back().seen = std::strtoull(v[1].c_str(), nullptr, 16);
                if (list.back().seen == ~0ull) list.back().seen = 0; // (written by an older version: read as never checked)
            } else if (!list.empty() && v[0] == "weapons" && v.size() >= 2) { // (its weapon slots are in the items)
                list.back().has_weapons = true;
                list.back().load_weapons = v[1] != "0";
                list.back().weapons_only = v[1] == "2";
            } else if (!list.empty() && (v[0] == "activity" || v[0] == "sleepwear") && v.size() >= 2) {
                unsigned x = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (x < kActivityCount) list.back().activity = static_cast<std::uint8_t>(x); // (sleepwear 1 = Sleeping)
            } else if (!list.empty() && v[0] == "weaponicon" && v.size() >= 2) {
                list.back().weapon_icon = v[1];
            } else if (!list.empty() && v[0] == "stash" && v.size() >= 2) {
                unsigned x = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (x <= 2) list.back().stash = static_cast<std::uint8_t>(x);
            } else if (!list.empty() && (v[0] == "climate" || v[0] == "keep") && v.size() >= 2) {
                unsigned x = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (v[0] == "climate" && x <= 3) list.back().climate = static_cast<std::uint8_t>(x);
                if (v[0] == "keep" && x <= 3) list.back().stash = x == 0 ? 0 : x == 1 ? 2 : 1; // (older: Keep in)
            } else if (!list.empty() && v[0] == "hotkey" && v.size() >= 3) {
                unsigned vk = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                unsigned m = static_cast<unsigned>(std::strtoul(v[2].c_str(), nullptr, 10));
                if (vk > 0 && vk < 256 && m < 8) list.back().hk_vk = static_cast<U16>(vk), list.back().hk_mods = static_cast<std::uint8_t>(m);
            } else if (!list.empty() && v[0] == "pin" && v.size() >= 2) { // (#32)
                const long p = std::strtol(v[1].c_str(), nullptr, 10);
                list.back().pin = p < 0 ? -1 : int(p);
            } else if (!list.empty() && v[0] == "used" && v.size() >= 2) {
                list.back().used = U32(std::strtoul(v[1].c_str(), nullptr, 10));
            } else if (!list.empty() && v[0] == "worn") {
                list.back().worn = true;
            } else if (!list.empty() && v[0] == "backlook" && v.size() >= 2) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                list.back().back_look = static_cast<std::uint8_t>(b < kLookCount ? b : 0);
            } else if (!list.empty() && v[0] == "underwear" && v.size() >= 2) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                list.back().underwear_look = static_cast<std::uint8_t>(b < kLookCount ? b : 0);
            } else if (!list.empty() && v[0] == "backshield" && v.size() >= 2) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                list.back().has_back = static_cast<std::uint8_t>(b <= 2 ? b : 0);
            } else if (!list.empty() && v[0] == "slotlook" && v.size() >= 3) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[2].c_str(), nullptr, 10));
                if (b < kLookCount) list.back().empty_looks[static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10))] = static_cast<std::uint8_t>(b);
            } else if (!list.empty() && v[0] == "item" && v.size() >= 4) {
                unsigned look = v.size() >= 5 ? static_cast<unsigned>(std::strtoul(v[4].c_str(), nullptr, 10)) : 0;
                list.back().items.push_back({static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10)),
                                             static_cast<U16>(std::strtoul(v[2].c_str(), nullptr, 10)),
                                             std::strtoull(v[3].c_str(), nullptr, 16),
                                             static_cast<std::uint8_t>(look < kLookCount ? look : 0),
                                             v.size() >= 6 ? static_cast<U32>(std::strtoul(v[5].c_str(), nullptr, 16)) : 0});
            }
        }
    }
    // Slot-only bindings (they were bound to the wrong slot): their lists go back to the active set wherever it has
    // none for that character, and the next save loaded takes them.
    for (const auto& [slot, s] : legacy)
        for (const CharacterOutfits& c : s)
            if (!c.outfits.empty() && store_for(c.character).outfits.empty()) {
                store_for(c.character).outfits = c.outfits;
                LOG("old save slot %u entry: %zu outfits for character %u back in the active set", slot,
                    c.outfits.size(), c.character);
            }
    for (const CharacterOutfits& c : g_store)
        if (!c.outfits.empty()) LOG("loaded %zu outfits for character %u", c.outfits.size(), c.character);
    for (const auto& [slot, b] : g_bound)
        LOG("save slot %u: %zu outfits (save size %llu, time %llX, hash %llX)", slot, outfit_total(b.store), b.id.size,
            b.id.time, b.id.hash);
    for (const PendingWrite& p : g_unfinished)
        LOG("save slot %u was still being written when the game closed: %zu outfits wait for it", p.slot,
            outfit_total(p.store));
    // Until a character is known the wheel shows the character-less (old) list.
    g_character = kNoCharacter;
    g_outfits = store_for(kNoCharacter).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    g_published = g_store;
    publish_looks(g_store);
}

// Makes `character`'s list the working one. Outfits saved before the lists were per character go to the
// first character seen (they were all made on the main character).
bool switch_character(U16 character)
{
    if (character == g_character) return false;
    abort_prompts("the character changed");
    if (g_character != kNoCharacter) store_for(g_character).outfits = g_outfits;
    CharacterOutfits& legacy = store_for(kNoCharacter);
    bool adopt = !legacy.outfits.empty() && store_for(character).outfits.empty();
    if (adopt) {
        store_for(character).outfits = legacy.outfits;
        legacy.outfits.clear();
    }
    LOG("character %u -> %u%s", g_character, character, adopt ? " (adopted the outfits without a character)" : "");
    g_character = character;
    g_outfits = store_for(character).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    if (adopt) save_outfits();
    return true;
}

} // namespace om
