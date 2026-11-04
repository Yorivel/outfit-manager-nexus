#include "resolver.h"

#include "game.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U8 = std::uint8_t;
using resolver::Alt;
using resolver::AltKind;
using resolver::How;
using resolver::Kind;
using resolver::Recipe;
using resolver::Result;

constexpr U32 kWithinWindow = 0x2000; // how far into a function a 'within' anchor looks (as tools/sigscan.py)
constexpr const char* kCacheHeader = "OutfitManager address cache v1";
// Cache proof codes besides an alt index: -1 = the first check (slot / prologue), kProofPattern = found by its own
// unique pattern (e.g. a slot function that is no longer in its slot), kProofShift = a thunk slot placed by the shift
// of its pattern-checked neighbours. (Misses are not cached: a miss can be temporary, e.g. another plugin hooking at
// start-up, so it is searched again next launch.)
constexpr int kProofPattern = -3;
constexpr int kProofShift = -4;

struct Pattern {
    std::vector<U8> bytes;
    std::vector<bool> mask; // true = must match
};

Pattern parse(const char* text)
{
    Pattern p;
    for (const char* c = text; c && *c;) {
        if (*c == ' ') {
            ++c;
        } else if (c[0] == '?' && c[1] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            c += 2;
        } else {
            char hex[3] = {c[0], c[1], 0};
            p.bytes.push_back(static_cast<U8>(std::strtoul(hex, nullptr, 16)));
            p.mask.push_back(true);
            c += 2;
        }
    }
    return p;
}

struct Image {
    U64 base = 0, size = 0;
    U64 ptr_base = 0; // what absolute pointers in the image are relative to (= base, except unrelocated test maps)
    U64 code = 0, code_size = 0; // the game's code: the largest executable, non-writable section
    U32 timestamp = 0, checksum = 0;
    U64 file_size = 0, file_time = 0;

    bool in_image(U64 rva, U64 len) const { return rva >= 0x1000 && rva + len <= size; }
    bool in_code(U64 rva, U64 len) const { return rva >= code && rva + len <= code + code_size; }
    const U8* at(U64 rva) const { return reinterpret_cast<const U8*>(base + rva); }

    bool match(U64 rva, const Pattern& p) const
    {
        if (p.bytes.empty() || !in_code(rva, p.bytes.size())) return false;
        const U8* b = at(rva);
        for (size_t i = 0; i < p.bytes.size(); ++i)
            if (p.mask[i] && b[i] != p.bytes[i]) return false;
        return true;
    }

    // rel32 / disp32 operand of the instruction at rva -> target rva (0 if outside the image).
    U64 decode(U64 rva, const Alt& a) const
    {
        if (!in_code(rva, a.end)) return 0;
        std::int32_t disp;
        std::memcpy(&disp, at(rva + a.operand), 4);
        U64 t = rva + a.end + static_cast<std::int64_t>(disp);
        return in_image(t, 1) ? t : 0;
    }
};

U64 g_test_base = 0; // resolver::set_test_image

Image load_image()
{
    Image img;
    img.base = g_test_base ? g_test_base : game::main_module().base;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
    img.size = nt->OptionalHeader.SizeOfImage;
    img.ptr_base = g_test_base ? nt->OptionalHeader.ImageBase : img.base;
    img.timestamp = nt->FileHeader.TimeDateStamp;
    img.checksum = nt->OptionalHeader.CheckSum;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        bool exec = sec->Characteristics & IMAGE_SCN_MEM_EXECUTE, write = sec->Characteristics & IMAGE_SCN_MEM_WRITE;
        if (exec && !write && sec->Misc.VirtualSize > img.code_size) {
            img.code = sec->VirtualAddress;
            img.code_size = sec->Misc.VirtualSize;
        }
    }
    wchar_t path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(g_test_base), path, MAX_PATH) && GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        img.file_size = (U64(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        img.file_time = (U64(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    return img;
}

U64 fnv(U64 h, const void* data, size_t n)
{
    auto p = static_cast<const U8*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

U64 fnv_str(U64 h, const char* s) { return fnv(h, s ? s : "", s ? std::strlen(s) + 1 : 1); }

U64 recipes_hash(const Recipe* r, size_t n)
{
    U64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h = fnv_str(h, r[i].name);
        h = fnv(h, &r[i].known, 4);
        h = fnv_str(h, r[i].pattern);
        h = fnv_str(h, r[i].check);
        h = fnv_str(h, r[i].rtti);
        h = fnv(h, &r[i].slot, 2);
        for (U8 k = 0; k < r[i].nalts; ++k) h = fnv_str(h, r[i].alts[k].pattern);
    }
    return h;
}

// --- Checks (cheap proofs) ------------------------------------------------------------------------

bool vtable_is(const Image& img, U64 rva, const char* rtti)
{
    if (!img.in_image(rva - 8, 8)) return false;
    U64 col = *reinterpret_cast<const U64*>(img.at(rva - 8)) - img.ptr_base;
    if (!img.in_image(col, 24)) return false;
    auto c = reinterpret_cast<const U32*>(img.at(col));
    if (c[0] != 1 || !img.in_image(c[3] + 0x10, std::strlen(rtti) + 1)) return false;
    return std::strcmp(reinterpret_cast<const char*>(img.at(c[3] + 0x10)), rtti) == 0;
}

// Slot content of a resolved vtable (0 if not a pointer into the image).
U64 slot_of(const Image& img, U64 vt, int slot)
{
    if (!img.in_image(vt + slot * 8, 8)) return 0;
    U64 p = *reinterpret_cast<const U64*>(img.at(vt + slot * 8));
    return p > img.ptr_base && p < img.ptr_base + img.size ? p - img.ptr_base : 0;
}

// The slot of a vtable that holds rva (the one nearest to `hint` if it is there several times), or -1. The vtable
// ends at the first entry that is not a pointer into the image.
int find_slot(const Image& img, U64 vt, U64 rva, int hint)
{
    int best = -1;
    for (int i = 0; i < 1024; ++i) {
        U64 p = slot_of(img, vt, i);
        if (!p) break; // (entries may point into other executable sections: thunks into protected code)
        if (p == rva && (best < 0 || std::abs(i - hint) < std::abs(best - hint))) best = i;
    }
    return best;
}

bool body_ok(const Image& img, const Recipe& r, U64 rva)
{
    if (r.pattern) return img.match(rva, parse(r.pattern));
    if (r.check) return img.match(rva, parse(r.check));
    if (r.kind == Kind::Thunk) return img.in_code(rva, 5) && *img.at(rva) == 0xE9;
    return true;
}

constexpr int kSlotReach = 8; // how far a virtual is looked for from its known slot (patches moved them by 1 to 4)

// Two vtables of one family: classes with the same base keep its virtuals, so they share entries it doesn't override,
// and a patch that adds a virtual to the base moves their slots alike (seen on the big rewrite: every UI class, slot
// 17-36 by 1, 47-139 by 2, 159+ by 4).
bool same_family(const Image& img, U64 a, U64 b)
{
    if (a == b) return true;
    int same = 0;
    for (int i = 0; i < 16; ++i) {
        U64 p = slot_of(img, a, i);
        same += p && p == slot_of(img, b, i);
    }
    return same >= 5;
}

// How many entries of a vtable hold this function (a stub the class shares among its slots shows up several times).
int slot_count(const Image& img, U64 vt, U64 rva)
{
    int n = 0;
    for (int i = 0; i < 1024; ++i) {
        U64 p = slot_of(img, vt, i);
        if (!p) break;
        n += p == rva;
    }
    return n;
}

// Anchor alt at instruction rva `site` really encodes `target`.
bool alt_at(const Image& img, const Alt& a, U64 site, U64 target)
{
    Pattern p = parse(a.pattern);
    U64 start = a.kind == AltKind::Ref ? site - a.site : site;
    return img.match(start, p) && img.decode(site, a) == target;
}

// The cheapest proof for one address. site/alt: the anchor that proved it last time (cache), or -1.
bool verify(const Image& img, const Recipe& r, const Result* res, U64 rva, U64 site, int alt)
{
    if (r.kind == Kind::Vtable && vtable_is(img, rva, r.rtti)) return true;
    if (r.kind == Kind::Vtable && !(alt >= 0 && alt < r.nalts && site)) return false;
    if (alt == kProofShift) { // placed by its neighbours' shift (its body may have changed): still in that vtable
        if (r.slot_vt < 0 || res[r.slot_vt].how == How::Missing) return false;
        return (r.kind != Kind::Thunk || *img.at(rva) == 0xE9) && find_slot(img, res[r.slot_vt].rva, rva, r.slot) >= 0;
    }
    if (r.check && !img.match(rva, parse(r.check))) return false;
    if (alt >= 0 && alt < r.nalts) return alt_at(img, r.alts[alt], site, rva); // the anchor that found it
    if (alt == kProofPattern) return r.pattern && img.match(rva, parse(r.pattern)); // found by its own pattern
    if (r.slot_vt >= 0) {
        const Result& vt = res[r.slot_vt];
        if (vt.how == How::Missing) return false;
        if (slot_of(img, vt.rva, r.slot) == rva && body_ok(img, r, rva)) return true;
        // its slot moved (the class gained or lost virtuals): still the right function if its pattern matches and
        // it is somewhere in the vtable
        return r.pattern && img.match(rva, parse(r.pattern)) && find_slot(img, vt.rva, rva, r.slot) >= 0;
    }
    return r.kind == Kind::Func && r.pattern && img.match(rva, parse(r.pattern));
}

// --- Search ---------------------------------------------------------------------------------------

// Calls fn(rva) for every occurrence of needle in the non-executable sections (RTTI names, type descriptors,
// object locators and vtables); fn returns false to stop.
template <class Fn>
void for_each_in_data(const Image& img, const void* needle, size_t len, Fn fn)
{
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    U8 first = *static_cast<const U8*>(needle);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        U64 va = sec->VirtualAddress, size = std::min<U64>(sec->Misc.VirtualSize, sec->SizeOfRawData);
        const U8* b = img.at(va);
        for (const U8* q = b; q + len <= b + size;) {
            q = static_cast<const U8*>(std::memchr(q, first, b + size - len + 1 - q));
            if (!q) break;
            if (std::memcmp(q, needle, len) == 0 && !fn(va + (q - b))) return;
            ++q;
        }
    }
}

// Primary vtable (object offset 0) of an RTTI type name, as an rva (0 if none).
U64 find_vtable(const Image& img, const char* rtti)
{
    U64 result = 0;
    for_each_in_data(img, rtti, std::strlen(rtti) + 1, [&](U64 name) {
        U32 td = U32(name - 0x10);
        // CompleteObjectLocator: {sig 1, offset, cdOffset, rvaTypeDescriptor, rvaClassDescriptor, rvaSelf}
        for_each_in_data(img, &td, 4, [&](U64 at) {
            auto col = reinterpret_cast<const U32*>(img.at(at - 12));
            if (col[0] != 1 || col[1] != 0 || col[5] != U32(at - 12)) return true;
            U64 col_va = img.ptr_base + at - 12;
            for_each_in_data(img, &col_va, 8, [&](U64 ref) {
                result = ref + 8;
                return false;
            });
            return result == 0;
        });
        return result == 0;
    });
    return result;
}

struct Hits {
    U32 count = 0;
    U64 first = 0; // rva of the first match
};

// One pass over the code section for many masked patterns at once: each pattern is keyed by its first fully fixed
// 4-byte window; a 64K-bit filter on that key keeps the per-byte cost to a load and a bit test.
void scan(const Image& img, const std::vector<Pattern>& pats, std::vector<Hits>& hits)
{
    hits.assign(pats.size(), {});
    struct Key { size_t pat; size_t off; };
    std::unordered_map<U32, std::vector<Key>> keys;
    std::vector<U64> filter(1024);
    std::vector<size_t> slow;
    auto hash = [](U32 k) { return (k * 2654435761u) >> 16; };
    for (size_t i = 0; i < pats.size(); ++i) {
        const Pattern& p = pats[i];
        size_t off = SIZE_MAX;
        for (size_t k = 0; k + 4 <= p.bytes.size(); ++k)
            if (p.mask[k] && p.mask[k + 1] && p.mask[k + 2] && p.mask[k + 3]) {
                off = k;
                break;
            }
        if (off == SIZE_MAX) {
            slow.push_back(i);
            continue;
        }
        U32 key;
        std::memcpy(&key, p.bytes.data() + off, 4);
        keys[key].push_back({i, off});
        U32 h = hash(key);
        filter[h >> 6] |= 1ull << (h & 63);
    }
    const U8* code = img.at(img.code);
    size_t len = img.code_size;
    auto try_at = [&](size_t i, size_t start) {
        const Pattern& p = pats[i];
        if (start + p.bytes.size() > len) return;
        for (size_t k = 0; k < p.bytes.size(); ++k)
            if (p.mask[k] && code[start + k] != p.bytes[k]) return;
        if (hits[i].count++ == 0) hits[i].first = img.code + start;
    };
    for (size_t pos = 0; pos + 4 <= len; ++pos) {
        U32 k;
        std::memcpy(&k, code + pos, 4);
        U32 h = hash(k);
        if (!(filter[h >> 6] & (1ull << (h & 63)))) continue;
        auto it = keys.find(k);
        if (it == keys.end()) continue;
        for (const Key& key : it->second)
            if (pos >= key.off) try_at(key.pat, pos - key.off);
    }
    for (size_t i : slow)
        for (size_t pos = 0; pos < len; ++pos) try_at(i, pos);
}

// k-th match of a short pattern within a function's first kWithinWindow bytes.
U64 within(const Image& img, U64 fn, const Pattern& p, U32 nth)
{
    U32 seen = 0;
    for (U64 off = 0; off < kWithinWindow; ++off)
        if (img.match(fn + off, p) && seen++ == nth) return fn + off;
    return 0;
}

// --- Cache ----------------------------------------------------------------------------------------

struct CacheEntry {
    U64 rva, site;
    int alt;
};

std::string identity(const Image& img, U64 rhash)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, "exe %08X %08X %llX %llX\nrecipes %016llX", img.timestamp, img.checksum,
                  img.file_size, img.file_time, rhash);
    return buf;
}

std::map<std::string, CacheEntry> read_cache(const wchar_t* path, const std::string& id)
{
    std::map<std::string, CacheEntry> out;
    FILE* f = nullptr;
    if (!path || _wfopen_s(&f, path, L"rb") != 0 || !f) return out;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    std::string head = std::string(kCacheHeader) + "\n" + id + "\n";
    if (text.compare(0, head.size(), head) != 0) {
        LOG("resolver: address cache is for another game build or plugin version; rebuilding it");
        return out;
    }
    size_t p = head.size();
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        char name[96];
        unsigned long long rva = 0, site = 0;
        int alt = -1;
        if (sscanf_s(line.c_str(), "%95s %llx %llx %d", name, unsigned(sizeof name), &rva, &site, &alt) == 4)
            out[name] = {rva, site, alt};
    }
    return out;
}

void write_cache(const wchar_t* path, const std::string& id, const Recipe* r, size_t n, const Result* res,
                 const std::vector<CacheEntry>& proof)
{
    std::wstring tmp = std::wstring(path) + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        LOG("resolver: cannot write the address cache");
        return;
    }
    std::fprintf(f, "%s\n%s\n", kCacheHeader, id.c_str());
    for (size_t i = 0; i < n; ++i)
        if (res[i].how != How::Missing && res[i].how != How::Conflict)
            std::fprintf(f, "%s %llx %llx %d\n", r[i].name, res[i].rva, proof[i].site, proof[i].alt);
    bool ok = std::fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        LOG("resolver: cannot replace the address cache");
        DeleteFileW(tmp.c_str());
    }
}

}

const char* resolver::how_name(How h)
{
    switch (h) {
    case How::Cached: return "cached";
    case How::Known: return "known";
    case How::Found: return "found";
    case How::Conflict: return "CONFLICT";
    default: return "MISSING";
    }
}

std::size_t resolver::resolve(const Recipe* r, std::size_t n, Result* res, const wchar_t* cache_path,
                              bool ignore_known)
{
    DWORD t0 = GetTickCount();
    Image img = load_image();
    std::string id = identity(img, recipes_hash(r, n));
    auto cache = ignore_known ? std::map<std::string, CacheEntry>{} : read_cache(cache_path, id);
    std::vector<CacheEntry> proof(n, CacheEntry{0, 0, -1});
    for (size_t i = 0; i < n; ++i) res[i] = {};

    // 1) cache, then the known build's addresses. Vtables first: slots depend on them (recipes are sorted by
    //    address, so do two rounds).
    size_t cached = 0, known = 0;
    for (int round = 0; round < 2 && !ignore_known; ++round)
        for (size_t i = 0; i < n; ++i) {
            if (res[i].how != How::Missing) continue;
            auto c = cache.find(r[i].name);
            if (c != cache.end() && verify(img, r[i], res, c->second.rva, c->second.site, c->second.alt)) {
                res[i] = {c->second.rva, How::Cached};
                proof[i] = c->second;
                ++cached;
                continue;
            }
            int alt = r[i].nalts && !(r[i].kind == Kind::Func && r[i].pattern) ? 0 : -1; // prologue first
            U64 site = alt >= 0 ? r[i].alts[0].known_site : 0;
            if (verify(img, r[i], res, r[i].known, site, alt)) {
                res[i] = {r[i].known, How::Known};
                proof[i] = {r[i].known, site, alt};
                ++known;
            }
        }

    // 2) search for the rest: every anchor of every unresolved address in one pass.
    std::vector<size_t> todo;
    for (size_t i = 0; i < n; ++i)
        if (res[i].how == How::Missing) todo.push_back(i);
    size_t found = 0;
    if (!todo.empty()) {
        std::vector<Pattern> pats;
        std::vector<std::vector<int>> pat_of(n); // per recipe: [own pattern or -1, alt patterns...]
        for (size_t i : todo) {
            pat_of[i].push_back(-1);
            if (r[i].kind == Kind::Func && r[i].pattern) { // (slot functions too: finds them if their slot moved)
                pat_of[i][0] = int(pats.size());
                pats.push_back(parse(r[i].pattern));
            }
            for (U8 k = 0; k < r[i].nalts; ++k) {
                pat_of[i].push_back(r[i].alts[k].kind == AltKind::Ref ? int(pats.size()) : -1);
                if (r[i].alts[k].kind == AltKind::Ref) pats.push_back(parse(r[i].alts[k].pattern));
            }
        }
        std::vector<Hits> hits;
        scan(img, pats, hits);
        // vtables by RTTI (data sections), then rounds for the dependent anchors (slots, within)
        for (size_t i : todo)
            if (r[i].kind == Kind::Vtable)
                if (U64 vt = find_vtable(img, r[i].rtti)) {
                    res[i] = {vt, How::Found, 1, 1};
                    ++found;
                }
        for (int round = 0; round < 6; ++round) {
            bool progress = false;
            for (size_t i : todo) {
                if ((r[i].kind == Kind::Vtable && !r[i].nalts) || res[i].how == How::Found || res[i].how == How::Conflict) continue;
                std::vector<std::pair<U64, CacheEntry>> got;
                U8 total = 0;
                bool waiting = false;
                // A thunk's only check is "E9 at the slot", which every thunk passes: its slot is placed by the
                // neighbours below (after the rounds), not taken as an anchor here.
                bool thunk_slot = r[i].slot_vt >= 0 && !r[i].pattern && r[i].kind == Kind::Thunk;
                if (r[i].slot_vt >= 0 && !thunk_slot) {
                    ++total;
                    const Result& vt = res[r[i].slot_vt];
                    if (vt.how == How::Missing || vt.how == How::Conflict) {
                        waiting = true;
                    } else if (U64 a = slot_of(img, vt.rva, r[i].slot); a && body_ok(img, r[i], a)) {
                        got.push_back({a, {a, 0, -1}});
                    } else if (r[i].check) { // the class gained or lost virtuals: its fingerprint elsewhere in the vtable
                        const Pattern fp = parse(r[i].check);
                        U64 only = 0;
                        int distinct = 0;
                        for (int s = std::max(0, r[i].slot - kSlotReach); s <= r[i].slot + kSlotReach; ++s) {
                            U64 p = slot_of(img, vt.rva, s);
                            if (!p) break;
                            if (p != only && img.match(p, fp)) only = p, ++distinct;
                        }
                        if (distinct == 1) got.push_back({only, {only, 0, -1}});
                    }
                }
                if (pat_of[i][0] >= 0) {
                    ++total;
                    const Hits& h = hits[pat_of[i][0]];
                    if (h.count == 1) got.push_back({h.first, {h.first, 0, kProofPattern}});
                }
                for (U8 k = 0; k < r[i].nalts; ++k) {
                    const Alt& a = r[i].alts[k];
                    ++total;
                    if (a.kind == AltKind::Ref) {
                        const Hits& h = hits[pat_of[i][k + 1]];
                        if (h.count != 1) continue;
                        U64 site = h.first + a.site;
                        U64 t = img.decode(site, a);
                        if (t && r[i].check && !img.match(t, parse(r[i].check))) {
                            LOG("resolver: %s: an anchor points at +%llX, which isn't that function; ignored", r[i].name, t);
                            t = 0;
                        }
                        if (t) got.push_back({t, {t, site, int(k)}});
                    } else {
                        const Result& fn = res[a.fn];
                        if (fn.how == How::Missing || fn.how == How::Conflict) {
                            waiting = true;
                            continue;
                        }
                        if (U64 site = within(img, fn.rva, parse(a.pattern), a.nth))
                            if (U64 t = img.decode(site, a); t && !(r[i].check && !img.match(t, parse(r[i].check))))
                                got.push_back({t, {t, site, int(k)}});
                    }
                }
                bool agree = !got.empty();
                for (auto& g : got) agree = agree && g.first == got[0].first;
                if (waiting && !agree) continue; // decide once every anchor could answer
                if (agree) {
                    res[i] = {got[0].first, How::Found, U8(got.size()), total};
                    proof[i] = got[0].second;
                    ++found;
                    progress = true;
                } else if (!got.empty()) {
                    res[i] = {0, How::Conflict, U8(got.size()), total};
                    std::string list;
                    char one[24];
                    for (auto& g : got) list += (std::snprintf(one, sizeof one, " +%llX", g.first), one);
                    LOG("resolver: %s: anchors disagree (%s); not used", r[i].name, list.c_str());
                } else if (!waiting) {
                    res[i].total = total;
                }
            }
            if (!progress) break;
        }
        // Slots no anchor could place (a thunk has nothing to check; a function may have been rewritten): taken only where
        // the nearest proven slot functions of the same vtable, below and above, moved by the same amount (0 = not at
        // all); no agreement there: of the whole family's vtables (closer neighbours). Slot 0 needs none below: nothing
        // goes before it. The slot that shift gives must hold a thunk (a thunk), or a
        // function the class has only there (a stub it shares among slots is not a real override).
        std::vector<bool> by_shift(n, false);
        for (size_t i : todo) {
            if (r[i].slot_vt < 0 || res[i].how == How::Found || res[i].how == How::Conflict) continue;
            const Result& vt = res[r[i].slot_vt];
            if (vt.how == How::Missing || vt.how == How::Conflict) continue;
            int lo = -1, hi = 1 << 30, lo_shift = 0, hi_shift = 0;
            auto agreed = [&] { return (lo >= 0 || r[i].slot == 0) && hi < (1 << 30) && lo_shift == hi_shift; };
            for (int pass = 0; pass < 2 && !agreed(); ++pass) { // (0: its own vtable, 1: its family, itself included)
                lo = -1, hi = 1 << 30, lo_shift = hi_shift = 0;
                for (size_t j = 0; j < n; ++j) {
                    bool good = res[j].how == How::Cached || res[j].how == How::Known || res[j].how == How::Found;
                    if (j == i || r[j].slot_vt < 0 || !(r[j].pattern || r[j].check) || !good || by_shift[j]) continue;
                    if (pass == 0 && r[j].slot_vt != r[i].slot_vt) continue;
                    const Result& other = res[r[j].slot_vt];
                    if (other.how == How::Missing || other.how == How::Conflict) continue;
                    if (pass == 1 && !same_family(img, vt.rva, other.rva)) continue;
                    int at = find_slot(img, other.rva, res[j].rva, r[j].slot);
                    if (at < 0) continue;
                    if (r[j].slot < r[i].slot && r[j].slot > lo) lo = r[j].slot, lo_shift = at - r[j].slot;
                    if (r[j].slot > r[i].slot && r[j].slot < hi) hi = r[j].slot, hi_shift = at - r[j].slot;
                }
            }
            U64 a = 0;
            if (agreed()) {
                a = slot_of(img, vt.rva, r[i].slot + hi_shift);
                bool thunk = a && img.in_code(a, 5) && *img.at(a) == 0xE9;
                if (a && (r[i].kind == Kind::Thunk ? !thunk : thunk || !img.in_code(a, 5) || slot_count(img, vt.rva, a) != 1)) a = 0;
            }
            if (a) {
                res[i] = {a, How::Found, 2, 2};
                proof[i] = {a, 0, kProofShift};
                by_shift[i] = true;
                ++found;
            } else {
                LOG("resolver: %s: its slot cannot be confirmed (neighbour slots %d / %d moved by %d / %d)", r[i].name,
                    lo, hi < (1 << 30) ? hi : -1, lo_shift, hi_shift);
            }
        }
    }

    size_t ok = 0;
    for (size_t i = 0; i < n; ++i) {
        bool good = res[i].how == How::Cached || res[i].how == How::Known || res[i].how == How::Found;
        ok += good;
        if (good && r[i].slot_vt >= 0 && res[r[i].slot_vt].how != How::Missing) {
            res[i].slot = static_cast<std::int16_t>(find_slot(img, res[r[i].slot_vt].rva, res[i].rva, r[i].slot));
            if (res[i].slot != r[i].slot)
                LOG("resolver: %s is in vtable slot %d now (was %d)", r[i].name, res[i].slot, r[i].slot);
        }
        long long moved = good ? static_cast<long long>(res[i].rva) - r[i].known : 0;
        char delta[32] = "";
        if (moved) std::snprintf(delta, sizeof delta, " moved %c%llX", moved < 0 ? '-' : '+', moved < 0 ? -moved : moved);
        if (res[i].how == How::Found || !good || ignore_known)
            LOG("resolver: %-26s %-8s +%llX%s (%u/%u anchors)%s", r[i].name, how_name(res[i].how), res[i].rva, delta,
                res[i].hit, res[i].total, ignore_known && good && moved ? "  <-- differs from the known build" : "");
    }
    LOG("resolver: %zu/%zu addresses (%zu cached, %zu known, %zu found by search) in %lu ms; exe %08X, code +%llX..+%llX",
        ok, n, cached, known, found, GetTickCount() - t0, img.timestamp, img.code, img.code + img.code_size);
    if (cache_path && !ignore_known && cached != n) write_cache(cache_path, id, r, n, res, proof);
    return ok;
}

std::size_t resolver::resolve_offsets(const Offset* o, std::size_t n, const Result* fns, std::uint64_t* out,
                                      OffsetStatus* status)
{
    Image img = load_image();
    std::size_t moved = 0, checked = 0;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = o[i].known;
        if (status) status[i] = OffsetStatus::Unchecked;
        std::vector<U64> vals;
        for (U8 k = 0; k < o[i].nanchors; ++k) {
            const OffAnchor& a = o[i].anchors[k];
            const Result& fn = fns[a.fn];
            if (fn.how == How::Missing || fn.how == How::Conflict) continue;
            U64 site = within(img, fn.rva, parse(a.pattern), a.nth);
            if (!site || !img.in_code(site + a.disp_at, a.disp_size)) continue;
            std::int64_t v;
            if (a.disp_size == 1) {
                v = *reinterpret_cast<const std::int8_t*>(img.at(site + a.disp_at));
            } else {
                std::int32_t d;
                std::memcpy(&d, img.at(site + a.disp_at), 4);
                v = d;
            }
            if (v > 0) vals.push_back(U64(v));
        }
        if (vals.empty()) {
            if (o[i].nanchors) LOG("resolver: offset %s: no witness found, keeping +%X", o[i].name, o[i].known);
            continue;
        }
        ++checked;
        if (status) status[i] = OffsetStatus::Doubtful; // until shown otherwise below
        bool agree = true;
        for (U64 v : vals) agree = agree && v == vals[0];
        if (!agree) {
            std::string list;
            char one[24];
            for (U64 v : vals) list += (std::snprintf(one, sizeof one, " +%llX", v), one);
            LOG("resolver: offset %s: witnesses disagree (%s), keeping +%X", o[i].name, list.c_str(), o[i].known);
        } else if (vals[0] != o[i].known && vals.size() < 2) {
            LOG("resolver: offset %s: one witness says +%llX, keeping +%X", o[i].name, vals[0], o[i].known);
        } else if (vals[0] != o[i].known) {
            LOG("resolver: offset %s moved +%X -> +%llX (%zu witnesses)", o[i].name, o[i].known, vals[0], vals.size());
            out[i] = vals[0];
            if (status) status[i] = OffsetStatus::Moved;
            ++moved;
        } else if (status) {
            status[i] = OffsetStatus::Same;
        }
    }
    LOG("resolver: %zu offsets, %zu checked against the game's code, %zu moved", n, checked, moved);
    return moved;
}

void resolver::set_test_image(std::uint64_t base)
{
    g_test_base = base;
}
