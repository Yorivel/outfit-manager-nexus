#include "uibin_patch.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

// Element format (no absolute offsets in the file):
//   classes   u8 flag; if 1: u32 n, n strings
//   children  u8 flag; if 1: u32 n, n elements
//   attrs     u8 flag; if 1: u32 n, n (key, value) strings
//   5 zero bytes
//   id, text / X, component, template, script: strings (u32 length + bytes)
//   u32, 3 tail bytes (kind, 0, 6)
// Elements are found by their id string, parsing back to where an element starting there ends just after it.
namespace uibin {
namespace {

using Bytes = std::vector<std::uint8_t>;
using SV = std::string_view;

struct Found {
    SV id;
    size_t start, end;
    bool child;            // has a parent (in the parse)
    size_t parent_start;   // the parent's start
    size_t parent_count;   // where the parent's child count is
};

bool u32(const Bytes& b, size_t i, std::uint32_t& v)
{
    if (i + 4 > b.size()) return false;
    std::memcpy(&v, b.data() + i, 4);
    return true;
}

bool str(const Bytes& b, size_t& i, SV* s = nullptr)
{
    std::uint32_t n;
    if (!u32(b, i, n) || n > 4096 || i + 4 + n > b.size()) return false;
    if (s) *s = SV(reinterpret_cast<const char*>(b.data() + i + 4), n);
    i += 4 + n;
    return true;
}

bool flag(const Bytes& b, size_t& i, bool& f)
{
    if (i >= b.size() || b[i] > 1) return false;
    f = b[i++] != 0;
    return true;
}

// The end of the element at i (false: no element there); `out` collects every element parsed.
bool element(const Bytes& b, size_t i, size_t& end, std::vector<Found>* out, bool child = false, size_t pstart = 0,
             size_t pcount = 0, int depth = 0)
{
    if (depth > 64) return false;
    size_t start = i;
    bool f;
    std::uint32_t n;
    if (!flag(b, i, f)) return false;
    if (f) {
        if (!u32(b, i, n) || n > 256) return false;
        i += 4;
        for (std::uint32_t k = 0; k < n; ++k)
            if (!str(b, i)) return false;
    }
    if (!flag(b, i, f)) return false;
    size_t count_at = i;
    if (f) {
        if (!u32(b, i, n) || n > 4096) return false;
        i += 4;
        for (std::uint32_t k = 0; k < n; ++k) {
            size_t e;
            if (!element(b, i, e, out, true, start, count_at, depth + 1)) return false;
            i = e;
        }
    }
    if (!flag(b, i, f)) return false;
    if (f) {
        if (!u32(b, i, n) || n > 512) return false;
        i += 4;
        for (std::uint32_t k = 0; k < 2 * n; ++k)
            if (!str(b, i)) return false;
    }
    if (i + 5 > b.size()) return false;
    for (int k = 0; k < 5; ++k)
        if (b[i + k]) return false;
    i += 5;
    SV id;
    if (!str(b, i, &id)) return false;
    for (int k = 0; k < 4; ++k)
        if (!str(b, i)) return false;
    i += 4;
    if (i + 3 > b.size() || b[i + 1] != 0 || b[i + 2] != 6) return false;
    i += 3;
    if (out) out->push_back({id, start, i, child, pstart, pcount});
    end = i;
    return true;
}

Bytes lp(SV s)
{
    Bytes r(4 + s.size());
    std::uint32_t n = std::uint32_t(s.size());
    std::memcpy(r.data(), &n, 4);
    std::memcpy(r.data() + 4, s.data(), s.size());
    return r;
}

size_t find(const Bytes& b, const Bytes& what, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min(to, b.size());
    if (from >= to) return SIZE_MAX;
    auto it = std::search(b.begin() + from, b.begin() + to, what.begin(), what.end());
    return it == b.begin() + to ? SIZE_MAX : size_t(it - b.begin());
}

size_t count(const Bytes& b, const Bytes& what, size_t from = 0, size_t to = SIZE_MAX)
{
    size_t n = 0;
    for (size_t at = find(b, what, from, to); at != SIZE_MAX; at = find(b, what, at + what.size(), to)) ++n;
    return n;
}

// Replaces the one occurrence of `old` in b (false when it isn't there exactly once).
bool replace_one(Bytes& b, const Bytes& old, const Bytes& with)
{
    size_t at = find(b, old);
    if (at == SIZE_MAX || find(b, old, at + old.size()) != SIZE_MAX) return false;
    b.erase(b.begin() + at, b.begin() + at + old.size());
    b.insert(b.begin() + at, with.begin(), with.end());
    return true;
}

void put32(Bytes& b, size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

// The element whose id string (length-prefixed) is at `at`: parse back from up to 3000 bytes before it.
bool element_of(const Bytes& b, size_t at, SV id, size_t& start, size_t& end, std::vector<Found>& found)
{
    size_t lo = at > 3000 ? at - 3000 : 0;
    for (size_t s = at; s-- > lo + 1;) { // (s from at-1 down to lo+1, as the tool's range(at - 1, lo, -1))
        if (b[s] != 1) continue;
        found.clear();
        size_t e;
        if (!element(b, s, e, &found)) continue;
        if (!found.empty() && found.back().id == id && found.back().start == s) {
            start = s, end = e;
            return true;
        }
    }
    return false;
}

constexpr SV kHeader = "QuickSlotHeader8", kNew = "QuickSlotHeaderOutfits";
constexpr SV kWrap = "CoolTimeChargedCountWrap", kText = "CoolTimeChargedCount", kValue = "OmValueText";

bool add_headers(Bytes& res, Report& r)
{
    Bytes old = lp(kHeader), neu = lp(kNew);
    // the tab bar, QuickSlotTabbarHeaderWrap, found by its last class and its child count flag (its earlier children
    // hold compiled script data the parser here doesn't read, so it can't be parsed from its start)
    Bytes parent = lp("quickslot-tabbar-header-wrap");
    parent.push_back(1);
    size_t pos = 0;
    for (;;) {
        size_t at = find(res, old, pos);
        if (at == SIZE_MAX) return true;
        size_t start, end;
        std::vector<Found> found;
        if (!element_of(res, at, kHeader, start, end, found)) return r.error = "no element for QuickSlotHeader8", false;
        size_t p = SIZE_MAX;
        for (size_t q = find(res, parent, start > 20000 ? start - 20000 : 0, start); q != SIZE_MAX;
             q = find(res, parent, q + 1, start))
            p = q;
        if (p == SIZE_MAX || count(res, parent, p, start) != 1) return r.error = "tab bar not found", false;
        size_t count_at = p + parent.size();
        std::uint32_t n;
        if (!u32(res, count_at, n) || n > 64) return r.error = "tab bar child count unreadable", false;
        Bytes copy(res.begin() + start, res.begin() + end);
        if (!replace_one(copy, old, neu)) return r.error = "header id not once in its element", false;
        res.insert(res.begin() + end, copy.begin(), copy.end());
        put32(res, count_at, n + 1);
        std::vector<Found> check;
        size_t e;
        if (!element(res, end, e, &check) || e != end + copy.size() || check.empty() || check.back().id != kNew ||
            check.back().start != end)
            return r.error = "the new header doesn't re-parse", false;
        pos = end + copy.size();
        ++r.headers;
    }
}

bool add_values(Bytes& res, Report& r)
{
    Bytes wrap = lp(kWrap);
    size_t pos = 0;
    for (;;) {
        size_t at = find(res, wrap, pos);
        if (at == SIZE_MAX) return true;
        size_t start, end;
        std::vector<Found> found;
        if (!element_of(res, at, kWrap, start, end, found)) return r.error = "no element for CoolTimeChargedCountWrap", false;
        const Found* kid = nullptr;
        int kids = 0;
        for (const Found& f : found)
            if (f.child && f.parent_start == start) ++kids, kid = &f;
        if (kids != 1 || kid->id != kText) return r.error = "the wrap doesn't hold just CoolTimeChargedCount", false;
        size_t cs = kid->start, ce = kid->end, count_at = kid->parent_count;
        Bytes copy(res.begin() + cs, res.begin() + ce);
        if (!replace_one(copy, lp(kText), lp(kValue)) || !replace_one(copy, lp("charged-count"), lp("om-value-text")) ||
            !replace_one(copy, lp("font-text-caption-white"), lp("font-text-caption-yellow")))
            return r.error = "CoolTimeChargedCount's id / classes changed", false;
        std::uint32_t n;
        if (!u32(res, count_at, n) || n != 1) return r.error = "the wrap's child count isn't 1", false;
        res.insert(res.begin() + ce, copy.begin(), copy.end());
        put32(res, count_at, n + 1);
        std::vector<Found> check;
        size_t e = 0;
        bool has = false;
        if (element(res, start, e, &check))
            for (const Found& f : check) has |= f.id == kValue;
        if (e != end + copy.size() || !has) return r.error = "the wrap doesn't re-parse", false;
        pos = end + copy.size();
        ++r.values;
    }
}

}

unsigned wants(const std::string& path)
{
    std::string p = path;
    for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t slash = p.find_last_of("/\\");
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    if (name == "itemquickslotview2_game.uibinary" || name == "quickslotitem_component_game.uibinary" ||
        name == "quickslotitem_game.uibinary")
        return 3;
    if (name == "itemquickslotview2_component_game.uibinary") return 2;
    return 0;
}

bool patch(const Bytes& in, unsigned what, Bytes& out, Report& r)
{
    out.clear();
    r = Report{};
    if (count(in, lp(kNew)) || count(in, lp(kValue))) return r.error = "already has the additions", false;
    Bytes res = in;
    if ((what & 1) && (!add_headers(res, r) || !r.headers)) {
        if (r.error.empty()) r.error = "no QuickSlotHeader8";
        return false;
    }
    if ((what & 2) && (!add_values(res, r) || !r.values)) {
        if (r.error.empty()) r.error = "no CoolTimeChargedCountWrap";
        return false;
    }
    out.swap(res);
    return true;
}

}
