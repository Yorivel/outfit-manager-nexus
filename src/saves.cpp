// Save binding: watching the game's save files and which save is loaded.
#include "settings.h"
#include "om.h"
#include "saves.h"
#include "store.h"
#include "gear.h"
#include "looks.h"
#include "pads.h"
#include "addrs.h"
#include <MinHook.h>

namespace om {

// --- Save binding: watching the game's save files (read only) -----------------------------------
// The game keeps each save in ...\save\<user id>\slot<N>\save.save (+ lobby.save). When the game writes a slot
// that save gets the active outfits of that moment. A save the game loads brings back the outfits of the save with the
// same content (any slot, so copied saves keep theirs); a save the mod never saw written starts with an empty wheel.
// The first save loaded takes the outfits made so far.
// Loads and writes are the game's own save requests (kRvaSaveCommit, hooked: commit_hook): a request reads its saves,
// checks them, writes, deletes - in that order, on the calling thread. One with nothing to write or delete is a load,
// applied as soon as it returns; one that writes binds its saves when it returns (the new save is in place: the game
// wrote slotN_new and renamed it to slotN). The save list (start-up, the Load menu) reads every save outside any
// request, and the read before a write is inside a writing request: neither is a load (2026-11-17; before, a read
// alone for 1.5 s was taken as a load and a write as done after 2 s of quiet).
// Without the request (not found after a game update) the old way stays: a read counts as a load only if no other
// slot is read within kListGapMs of it and it stays alone for kLoadRestMs; a write (seen by watching the save folder)
// is bound once the file stays unchanged for kWriteRestMs.
constexpr U64 kListGapMs = 500;
constexpr U64 kLoadRestMs = 1500;
constexpr U64 kWriteRestMs = 2000;
bool g_commit_hooked = false; // (set once at start-up, before the game's first save request)

struct PendingLoad {
    int slot = -1;
    std::wstring path;
    SaveId id; // size + time when read
    U64 at = 0;
};
PendingLoad g_pending_load;
int g_last_read_slot = -1;
U64 g_last_read_at = 0;
std::atomic<bool> g_save_work{false}; // a load or write is pending
std::wstring g_save_root;             // ...\save\<user id>\ (set once, before the watch thread starts)
std::atomic<bool> g_watching{false};
thread_local bool t_in_file_hook = false; // our own file accesses are not the game's

SaveId quick_id(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return {};
    return {(U64(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
            (U64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime, 0};
}

// FNV-1a over the whole file, opened read only and shared with the game; 0 if it cannot be read.
U64 hash_file(const std::wstring& path)
{
    bool nested = t_in_file_hook;
    t_in_file_hook = true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    t_in_file_hook = nested;
    if (h == INVALID_HANDLE_VALUE) return 0;
    U64 v = 0xCBF29CE484222325ull;
    std::vector<unsigned char> buf(1 << 16);
    DWORD n = 0;
    while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n)
        for (DWORD i = 0; i < n; ++i) v = (v ^ buf[i]) * 0x100000001B3ull;
    CloseHandle(h);
    return v ? v : 1;
}

// The save's clock and place (savekey.cpp), our own read of the file like the hash.
void fill_key(SaveId& id, const std::wstring& path)
{
    bool nested = t_in_file_hook;
    t_in_file_hook = true;
    if (!read_save_key(path, id.clock, id.place)) id.clock = 0, id.place = 0;
    t_in_file_hook = nested;
}

// "...\save\<digits>\slot<N>\<file>" -> N, else -1. `dir` gets the path up to and including the last separator.
// `staged` (if given): also "slot<N>_new" (the folder a save is written in before it replaces slot<N>) -> N, set true.
int save_slot(const wchar_t* path, std::wstring* file, std::wstring* dir, bool* staged = nullptr)
{
    if (!path || !wcsstr(path, L"slot")) return -1;
    std::wstring p = path;
    for (wchar_t& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    size_t f = p.rfind(L'\\');
    if (f == std::wstring::npos || f == 0) return -1;
    size_t d = p.rfind(L'\\', f - 1);
    if (d == std::wstring::npos || d == 0 || p.compare(d + 1, 4, L"slot") != 0) return -1;
    size_t u = p.rfind(L'\\', d - 1);
    if (u == std::wstring::npos || u + 1 == d || u < 5 || p.compare(u - 5, 5, L"\\save") != 0) return -1;
    for (size_t i = u + 1; i < d; ++i)
        if (!std::iswdigit(p[i])) return -1;
    if (d + 5 >= f) return -1;
    size_t digits_end = f;
    if (staged) {
        *staged = f > d + 9 && p.compare(f - 4, 4, L"_new") == 0;
        if (*staged) digits_end = f - 4;
    }
    if (digits_end <= d + 5) return -1;
    for (size_t i = d + 5; i < digits_end; ++i)
        if (!std::iswdigit(p[i])) return -1;
    if (file) *file = p.substr(f + 1);
    if (dir) *dir = std::wstring(path, f + 1);
    return static_cast<int>(std::wcstoul(p.c_str() + d + 5, nullptr, 10));
}

// "slot<N>" or "slot<N>\..." (a name relative to g_save_root) -> N, else -1.
int slot_of_name(const std::wstring& name)
{
    if (name.size() < 5 || _wcsnicmp(name.c_str(), L"slot", 4) != 0) return -1;
    size_t i = 4;
    while (i < name.size() && std::iswdigit(name[i])) ++i;
    if (i == 4 || (i < name.size() && name[i] != L'\\')) return -1;
    return static_cast<int>(std::wcstoul(name.c_str() + 4, nullptr, 10));
}

std::wstring slot_save_path(U32 slot) { return g_save_root + L"slot" + std::to_wstring(slot) + L"\\save.save"; }

// Outfits carried into a save from elsewhere (a save the mod never saw written, or Outfits: Same in every save): what
// belongs to one save is dropped - which outfit was applied, the helmet key's pin, the No Outfit look (learnt from
// this save once in the game) (2026-11-13: a player's saves made while the mod was off came up with no outfits).
void carry_over(Store& s)
{
    for (CharacterOutfits& c : s) {
        for (Outfit& o : c.outfits) o.worn = false;
        c.helmet_pin = 0;
        c.look_base.clear();
    }
}

// The pending read becomes the active set (g_store_mu held).
constexpr U64 kNewGameSaveMax = 64 * 1024; // (a save this small is a new game's starting save)
void apply_load(const char* why)
{
    PendingLoad l = g_pending_load;
    g_pending_load.slot = -1;
    if (l.slot < 0) return;
    U32 slot = static_cast<U32>(l.slot);
    SaveId id = l.id;
    SaveId now = quick_id(l.path);
    if (now.size == id.size && now.time == id.time) id.hash = hash_file(l.path), fill_key(id, l.path); // else rewritten
                                                                                                      // since: size + time
    const char* how;
    auto pw = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                           [&](const PendingWrite& p) { return p.slot == slot; });
    int from = -1;
    for (const auto& [s, b] : g_bound)
        if (b.id.same(id) && (from < 0 || s == slot)) from = static_cast<int>(s);
    // Not the same bytes, but the same moment: the save the mod bound, changed since by something else (a save editor,
    // 2026-11-12 report) - same world clock and place, so its outfits (an older save of the playthrough has another clock).
    bool edited = false;
    if (from < 0 && id.clock)
        for (const auto& [s, b] : g_bound)
            if (b.id.same_moment(id) && (from < 0 || s == slot)) from = static_cast<int>(s), edited = true;
    // A new game: the game writes a fresh starting save (about 5 KB; a played save is hundreds of KB and more) and loads
    // it at once - no save of the player's is read. Without this it took the outfits of the save played before, and
    // the new game's first autosave kept them (2026-11-06).
    U64 size = now.size ? now.size : id.size;
    if (g_shared_outfits) { // (Outfits: Same in every save - the current ones, whatever this save had)
        carry_over(g_published);
        g_bound[slot] = {id, g_published};
        how = "Outfits: Same in every save - the current outfits";
    } else if (size && size < kNewGameSaveMax) {
        g_published.clear();
        if (pw != g_pending_writes.end()) pw->store.clear(); // (the starting save itself keeps none either)
        g_bound[slot] = {id, g_published};
        how = "a new game's starting save: no outfits";
    } else if (pw != g_pending_writes.end()) {
        g_published = pw->store;
        how = "restored (saved moments ago)";
    } else if (g_bound.empty()) {
        g_bound[slot] = {id, g_published};
        how = "first save loaded with the mod: it keeps the current outfits";
    } else if (from >= 0) {
        g_published = g_bound[U32(from)].store;
        g_bound[slot] = {id, g_published};
        how = edited ? "restored (the same save, changed outside the game: its world clock and place match)"
                     : U32(from) == slot ? "restored" : "restored (same save as another slot)";
    } else { // (made while the mod was off, or elsewhere: the current outfits rather than none)
        carry_over(g_published);
        g_bound[slot] = {id, g_published};
        how = "the mod never saw this save written: it takes the current outfits";
    }
    g_loaded_id = id, g_loaded_known = true;
    ++g_generation;
    publish_looks(g_published); // (before the game builds the character: its looks apply from the loading screen)
    write_store_file();
    LOG("game loaded save slot %u (%s; size %llu, hash %llX): %s -> %zu outfits", slot, why, id.size, id.hash, how,
        outfit_total(g_published));
}

void on_save_read(U32 slot, const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    U64 now = GetTickCount64();
    bool listing = g_last_read_slot >= 0 && U32(g_last_read_slot) != slot && now - g_last_read_at < kListGapMs;
    g_last_read_slot = static_cast<int>(slot);
    g_last_read_at = now;
    if (listing) {
        if (g_pending_load.slot >= 0) LOG("game is listing its saves (start-up / load menu): not a load");
        g_pending_load.slot = -1;
        return;
    }
    g_pending_load = {static_cast<int>(slot), path, quick_id(path), now};
    g_save_work = true;
}

// The game began writing save slot N: it gets the outfits active now (g_store_mu held). False: already noted.
bool begin_write(U32 slot, const wchar_t* what, DWORD action)
{
    if (std::any_of(g_pending_writes.begin(), g_pending_writes.end(), [&](const PendingWrite& p) { return p.slot == slot; }))
        return false;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    Store with = g_published;
    record_worn_for_save(with); // (the applied outfit as the equipment says, not a swap's moment - gear.cpp)
    g_pending_writes.push_back(
        {slot, slot_save_path(slot), with, {}, GetTickCount64(), (U64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime});
    write_store_file(); // noted at once, in case the game closes before the write is bound
    LOG("game is writing save slot %u (%ls, change %lu): %zu outfits go with it", slot, what, action,
        outfit_total(g_published));
    return true;
}

// A written save gets its outfits (g_store_mu held): its id from the file as it is now; gone = its outfits dropped.
void bind_written(PendingWrite& w)
{
    SaveId q = quick_id(w.path);
    if (!q.size) {
        if (g_bound.erase(w.slot)) LOG("save slot %u is gone: its outfits are dropped", w.slot);
        return;
    }
    q.hash = hash_file(w.path);
    fill_key(q, w.path);
    LOG("save slot %u written: %zu outfits bound (size %llu, hash %llX)", w.slot, outfit_total(w.store), q.size, q.hash);
    g_bound[w.slot] = {q, std::move(w.store)};
}

// Watch thread: something in slot<N>\ changed (the game is writing that save, or deleting it). (Only without the
// game's save requests: g_commit_hooked.)
void on_slot_changed(U32 slot, const std::wstring& name, DWORD action)
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_pending_load.slot >= 0) {
        // The game reads a save it is about to overwrite (overwrite check / backup): that read is not a load. (The read
        // is at most kLoadRestMs old here; a real load is never followed that fast by a write of the same slot. The
        // file name is not checked: the write may start with a temp file.)
        if (U32(g_pending_load.slot) == slot) {
            LOG("save slot %u was read just before being written: not a load", slot);
            g_pending_load.slot = -1;
        } else {
            apply_load("before a save write");
        }
    }
    if (!begin_write(slot, name.c_str(), action))
        for (PendingWrite& p : g_pending_writes)
            if (p.slot == slot) p.since = GetTickCount64();
    g_save_work = true;
}

// Watch thread start: saves the game was still writing when it last closed get their outfits now, if the save file
// did change since the write began (else the write never finished and the old save keeps its own).
void bind_unfinished()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_unfinished.empty()) return;
    constexpr U64 kSlack = 10'000'000; // 1 s in FILETIME units
    for (const PendingWrite& p : g_unfinished) {
        std::wstring path = slot_save_path(p.slot);
        SaveId id = quick_id(path);
        if (id.size && id.time + kSlack >= p.started) {
            id.hash = hash_file(path);
            fill_key(id, path);
            g_bound[p.slot] = {id, p.store};
            LOG("save slot %u, written as the game closed: %zu outfits bound (size %llu, hash %llX)", p.slot,
                outfit_total(p.store), id.size, id.hash);
        } else {
            LOG("save slot %u was not finished when the game closed: its outfits are dropped", p.slot);
        }
    }
    g_unfinished.clear();
    write_store_file();
}

// Saves bound before the save key existed (or unreadable then): their key, from the file if it is still the bound one.
void key_bound_saves()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    int keyed = 0;
    for (auto& [slot, b] : g_bound) {
        if (b.id.clock || !b.id.hash) continue;
        const std::wstring path = slot_save_path(slot);
        if (hash_file(path) != b.id.hash) continue; // (changed since: it gets a key when next written)
        fill_key(b.id, path);
        keyed += b.id.clock != 0;
    }
    if (keyed) {
        LOG("save keys: %d bound save(s) got their world clock and place", keyed);
        write_store_file();
    }
}

DWORD WINAPI save_watch_thread(LPVOID)
{
    t_in_file_hook = true;
    bind_unfinished();
    key_bound_saves();
    if (g_commit_hooked) return 0; // (writes are seen in the game's own save requests: no folder watch)
    HANDLE dir = CreateFileW(g_save_root.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        LOG("cannot watch %ls (%lu): outfits will not follow the game saves", g_save_root.c_str(), GetLastError());
        return 0;
    }
    LOG("watching %ls for save writes", g_save_root.c_str());
    std::vector<DWORD> buf(16384 / sizeof(DWORD));
    DWORD n = 0;
    while (ReadDirectoryChangesW(dir, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE,
                                 &n, nullptr, nullptr)) {
        if (n == 0) continue; // overflow: the next change of the slot still arrives
        auto* e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());
        for (;;) {
            std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
            if (int slot = slot_of_name(name); slot >= 0) on_slot_changed(U32(slot), name, e->Action);
            if (!e->NextEntryOffset) break;
            e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(e) + e->NextEntryOffset);
        }
    }
    LOG("save watch stopped (%lu)", GetLastError());
    CloseHandle(dir);
    return 0;
}

// Key thread, every 250 ms: apply a read as a load once it has rested; bind a written save once it stays put.
void poll_saves()
{
    if (!g_save_work) return;
    std::lock_guard<std::mutex> lock(g_store_mu);
    U64 now = GetTickCount64();
    if (g_pending_load.slot >= 0 && now - g_pending_load.at >= kLoadRestMs) apply_load("read alone");
    bool changed = false;
    for (auto it = g_pending_writes.begin(); it != g_pending_writes.end();) {
        SaveId q = quick_id(it->path);
        if (q.size != it->id.size || q.time != it->id.time) {
            it->id = q;
            it->since = now;
        }
        if (now - it->since < kWriteRestMs) {
            ++it;
            continue;
        }
        bind_written(*it);
        changed = true;
        it = g_pending_writes.erase(it);
    }
    if (changed) write_store_file();
    g_save_work = g_pending_load.slot >= 0 || !g_pending_writes.empty();
}

// UI thread, menu opening: a read that has been alone for a while is certainly a load by now.
void settle_load_now()
{
    if (!g_save_work) return;
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_pending_load.slot >= 0 && GetTickCount64() - g_pending_load.at >= kListGapMs) apply_load("menu opened");
}

// --- The game's save requests (kRvaSaveCommit) ------------------------------------------------------------------
// One request on the calling thread: its reads (pa::GamePlaySaveDataFileHandler slot 4, "Commit Load"), a check, its
// writes (slot 5, when +kOffSaveReqWrites) and deletes (slot 6, when +kOffSaveReqDeletes); false if one failed (the
// game then puts the old save folder back). The save files it opens are seen by the file hooks below, on this thread.
using CommitFn = bool(__fastcall*)(U64 self, U64 request);
CommitFn g_commit_orig = nullptr;
enum CommitKind { kNoCommit, kLoadCommit, kWriteCommit };
thread_local int t_commit = kNoCommit;
thread_local int t_commit_read = -1;     // (a load: the slot whose save.save it read)
thread_local U32 t_commit_written[8];     // (a write: the slots whose save.save it wrote)
thread_local int t_commit_written_n = 0;

// The request is over (not on the game's file path any more: our own file reads are fine here).
void commit_done(bool ok, U32 writes, U32 deletes, int read, const U32* written, int written_n)
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (!writes && !deletes) {
        if (read < 0) return; // (nothing of ours: lobby.save alone, or another file)
        if (!ok) {
            LOG("the game's load of save slot %d failed: the outfits stay as they are", read);
            return;
        }
        const std::wstring path = slot_save_path(U32(read));
        g_pending_load = {read, path, quick_id(path), GetTickCount64()};
        apply_load("the game's load");
        return;
    }
    LOG("the game's save request is over (%s; %u to write, %u to delete)", ok ? "done" : "FAILED", writes, deletes);
    bool changed = false;
    for (int i = 0; i < written_n; ++i) {
        auto it = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                               [&](const PendingWrite& p) { return p.slot == written[i]; });
        if (it == g_pending_writes.end()) continue;
        if (ok) bind_written(*it);
        else LOG("save slot %u: the game's write failed (it puts the old save back): its outfits stay as they were", it->slot);
        g_pending_writes.erase(it);
        changed = true;
    }
    if (writes && !written_n) LOG("the game wrote a save without opening a save.save the mod knows: nothing bound");
    if (deletes && ok) // (a deleted save: its folder is gone)
        for (auto it = g_bound.begin(); it != g_bound.end();)
            if (!quick_id(slot_save_path(it->first)).size) {
                LOG("save slot %u was deleted: its outfits are dropped", it->first);
                it = g_bound.erase(it);
                changed = true;
            } else {
                ++it;
            }
    if (changed) write_store_file();
}

bool __fastcall commit_hook(U64 self, U64 request)
{
    U32 writes = 0, deletes = 0;
    __try {
        writes = *reinterpret_cast<const U32*>(request + kOffSaveReqWrites);
        deletes = *reinterpret_cast<const U32*>(request + kOffSaveReqDeletes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_commit_orig(self, request);
    }
    const int outer = t_commit; // (one request inside another: not seen, but kept apart if it happens)
    t_commit = writes || deletes ? kWriteCommit : kLoadCommit;
    t_commit_read = -1, t_commit_written_n = 0;
    const bool ok = g_commit_orig(self, request);
    const int read = t_commit_read, written_n = t_commit_written_n;
    U32 written[8];
    std::copy(t_commit_written, t_commit_written + written_n, written);
    t_commit = outer, t_commit_read = -1, t_commit_written_n = 0;
    commit_done(ok, writes, deletes, read, written, written_n);
    return ok;
}

// A game open of a save file. With the game's save requests: a read inside a load request is the load (applied when it
// returns), any other read is the save list or the read before a write; a write inside a writing request (into
// slotN_new) begins that slot's write. Without them, the old way: reads of save.save are load candidates. The first
// open also starts the watch thread (outfits of writes left unfinished, save keys; the folder watch without requests).
void on_save_file(const wchar_t* path, DWORD access, bool ok, const char* api)
{
    std::wstring file, dir;
    bool staged = false;
    int slot = save_slot(path, &file, &dir, &staged);
    if (slot < 0 || !ok || file != L"save.save") return;
    if (!g_watching.exchange(true)) {
        g_save_root = dir.substr(0, dir.size() - 1);
        g_save_root.resize(g_save_root.find_last_of(L"\\/") + 1);
        if (HANDLE t = CreateThread(nullptr, 0, &save_watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    const bool reads = (access & (GENERIC_READ | FILE_READ_DATA)) && !(access & (GENERIC_WRITE | FILE_WRITE_DATA));
    if (g_commit_hooked) {
        if (reads && !staged) {
            LOG("%s read slot%d\\save.save (%s)", api, slot,
                t_commit == kLoadCommit ? "the game's load" : t_commit == kWriteCommit ? "before a save write" : "the save list");
            if (t_commit == kLoadCommit) t_commit_read = slot;
        } else if (!reads && t_commit == kWriteCommit) {
            std::lock_guard<std::mutex> lock(g_store_mu);
            begin_write(U32(slot), path, 0);
            if (t_commit_written_n < 8 &&
                std::find(t_commit_written, t_commit_written + t_commit_written_n, U32(slot)) == t_commit_written + t_commit_written_n)
                t_commit_written[t_commit_written_n++] = U32(slot);
        }
        return;
    }
    if (!reads || staged) return;
    LOG("%s read slot%d\\save.save", api, slot);
    on_save_read(U32(slot), dir + L"save.save");
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
CreateFileWFn g_create_file_orig = nullptr;
CreateFile2Fn g_create_file2_orig = nullptr;

HANDLE WINAPI create_file_hook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_create_file_orig(path, access, share, sa, disposition, flags, tmpl);
    if (!t_in_file_hook && path && (path[0] == L'\\' || path[0] == L'/') && (wcsstr(path, L"hid#") || wcsstr(path, L"HID#"))) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        note_pad_handle(path, h);
        t_in_file_hook = false;
        SetLastError(err);
    }
    if (!t_in_file_hook && path && wcsstr(path, L"slot")) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        on_save_file(path, access, h != INVALID_HANDLE_VALUE, "CreateFileW");
        t_in_file_hook = false;
        SetLastError(err);
    }
    return h;
}

HANDLE WINAPI create_file2_hook(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                LPCREATEFILE2_EXTENDED_PARAMETERS params)
{
    HANDLE h = g_create_file2_orig(path, access, share, disposition, params);
    if (!t_in_file_hook && path && wcsstr(path, L"slot")) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        on_save_file(path, access, h != INVALID_HANDLE_VALUE, "CreateFile2");
        t_in_file_hook = false;
        SetLastError(err);
    }
    return h;
}

bool install_save_hooks()
{
    struct { const char* name; void* fn; void** orig; } hooks[] = {
        {"CreateFileW", reinterpret_cast<void*>(&create_file_hook), reinterpret_cast<void**>(&g_create_file_orig)},
        {"CreateFile2", reinterpret_cast<void*>(&create_file2_hook), reinterpret_cast<void**>(&g_create_file2_orig)},
    };
    for (auto& h : hooks) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(L"kernelbase", h.name, h.fn, h.orig, &target) != MH_OK || MH_EnableHook(target) != MH_OK) {
            LOG("save hook %s failed", h.name);
            return false;
        }
    }
    // the game's save requests (see commit_hook); not found or not hooked: loads and writes are told the old way
    void* t = reinterpret_cast<void*>(g_base + kRvaSaveCommit);
    g_commit_hooked = have({kRvaSaveCommit, kOffSaveReqWrites, kOffSaveReqDeletes}) &&
                      MH_CreateHook(t, reinterpret_cast<void*>(&commit_hook), reinterpret_cast<void**>(&g_commit_orig)) == MH_OK &&
                      MH_EnableHook(t) == MH_OK;
    if (g_commit_hooked) LOG("saves: loads and writes from the game's save requests (+%llX)", kRvaSaveCommit);
    else LOG("saves: the game's save requests weren't found; loads and writes are told by file timing");
    return true;
}

} // namespace om
