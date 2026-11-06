// Save binding: watching the game's save files and which save is loaded.
#include "om.h"
#include "saves.h"
#include "store.h"
#include "om_rest.h"

namespace om {

// --- Save binding: watching the game's save files (read only) -----------------------------------
// The game keeps each save in ...\save\<user id>\slot<N>\save.save (+ lobby.save). When the game writes a slot (seen
// by watching the save folder, whatever API it uses) that save gets the active outfits of that moment. A save the
// game loads brings back the outfits of the save with the same content (any slot, so copied saves keep theirs);
// a save the mod never saw written starts with an empty wheel. The first save loaded takes the outfits made so far.
// Loads: the game reads every save to list them (start-up, load menu), slots a few ms apart; a load reads one slot
// alone. So a read counts as a load only if no other slot is read within kListGapMs of it.
constexpr U64 kListGapMs = 500;
constexpr U64 kLoadRestMs = 1500; // a read becomes a load after this long alone
constexpr U64 kWriteRestMs = 2000; // a written save is hashed once it stays unchanged this long

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

// "...\save\<digits>\slot<N>\<file>" -> N, else -1. `dir` gets the path up to and including the last separator.
int save_slot(const wchar_t* path, std::wstring* file, std::wstring* dir)
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
    for (size_t i = d + 5; i < f; ++i)
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

// The pending read becomes the active set (g_store_mu held).
void apply_load(const char* why)
{
    PendingLoad l = g_pending_load;
    g_pending_load.slot = -1;
    if (l.slot < 0) return;
    U32 slot = static_cast<U32>(l.slot);
    SaveId id = l.id;
    SaveId now = quick_id(l.path);
    if (now.size == id.size && now.time == id.time) id.hash = hash_file(l.path); // else rewritten since: size + time
    const char* how;
    auto pw = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                           [&](const PendingWrite& p) { return p.slot == slot; });
    int from = -1;
    for (const auto& [s, b] : g_bound)
        if (b.id.same(id) && (from < 0 || s == slot)) from = static_cast<int>(s);
    if (pw != g_pending_writes.end()) {
        g_published = pw->store;
        how = "restored (saved moments ago)";
    } else if (g_bound.empty()) {
        g_bound[slot] = {id, g_published};
        how = "first save loaded with the mod: it keeps the current outfits";
    } else if (from >= 0) {
        g_published = g_bound[U32(from)].store;
        g_bound[slot] = {id, g_published};
        how = U32(from) == slot ? "restored" : "restored (same save as another slot)";
    } else {
        g_published.clear();
        how = "the mod never saw this save written: no outfits";
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

// Watch thread: something in slot<N>\ changed (the game is writing that save, or deleting it).
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
    U64 now = GetTickCount64();
    auto it = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                           [&](const PendingWrite& p) { return p.slot == slot; });
    if (it == g_pending_writes.end()) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        g_pending_writes.push_back(
            {slot, slot_save_path(slot), g_published, {}, now, (U64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime});
        write_store_file(); // noted at once, in case the game closes before the write is bound
        LOG("game is writing save slot %u (%ls, change %lu): %zu outfits go with it", slot, name.c_str(), action,
            outfit_total(g_published));
    } else {
        it->since = now;
    }
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

DWORD WINAPI save_watch_thread(LPVOID)
{
    t_in_file_hook = true;
    bind_unfinished();
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
        if (!q.size) {
            if (g_bound.erase(it->slot)) LOG("save slot %u is gone: its outfits are dropped", it->slot);
        } else {
            q.hash = hash_file(it->path);
            LOG("save slot %u written: %zu outfits bound (size %llu, hash %llX)", it->slot, outfit_total(it->store),
                q.size, q.hash);
            g_bound[it->slot] = {q, std::move(it->store)};
        }
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

// A game open of a save file: reads of save.save are load candidates; the first one also starts the folder watch.
void on_save_file(const wchar_t* path, DWORD access, bool ok, const char* api)
{
    std::wstring file, dir;
    int slot = save_slot(path, &file, &dir);
    if (slot < 0 || !ok || file != L"save.save") return;
    if (!g_watching.exchange(true)) {
        g_save_root = dir.substr(0, dir.size() - 1);
        g_save_root.resize(g_save_root.find_last_of(L"\\/") + 1);
        if (HANDLE t = CreateThread(nullptr, 0, &save_watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (!(access & (GENERIC_READ | FILE_READ_DATA)) || (access & (GENERIC_WRITE | FILE_WRITE_DATA))) return;
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
    return true;
}

} // namespace om
