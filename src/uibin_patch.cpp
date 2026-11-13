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

// The element whose id string (length-prefixed) is at `at`: parse back from up to 12000 bytes before it (the id comes
// after the children: a tracker is ~3000 bytes).
bool element_of(const Bytes& b, size_t at, SV id, size_t& start, size_t& end, std::vector<Found>& found)
{
    size_t lo = at > 12000 ? at - 12000 : 0;
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

bool set_classes(Bytes& e, const std::vector<std::string>& cls);
int replace_all(Bytes& b, const Bytes& old, const Bytes& with);

// `e` (an element with no children) with `kids` (n elements, back to back) as its children.
bool give_children(Bytes& e, const Bytes& kids, std::uint32_t n)
{
    size_t i = 5;
    std::uint32_t nc = 0;
    if (e.size() < 5 || e[0] != 1 || !u32(e, 1, nc)) return false;
    for (std::uint32_t k = 0; k < nc; ++k)
        if (!str(e, i)) return false;
    if (i >= e.size() || e[i] != 0) return false;
    e[i] = 1;
    Bytes cnt(4);
    std::memcpy(cnt.data(), &n, 4);
    e.insert(e.begin() + i + 1, cnt.begin(), cnt.end());
    e.insert(e.begin() + i + 5, kids.begin(), kids.end());
    size_t end = 0;
    return element(e, 0, end, nullptr) && end == e.size();
}

// An outfit's hotkey as the game's key caps (2026-11-06): OmHkCaps (om-hk-caps, hidden until shown) holding seven
// copies of the slot's text element: three modifier caps with "+" after each, then the key's cap (om-hk-c0..c6); the
// plugin sets their images (Common_Icon_Keyguide_PC_*) and hides the unused ones.
bool make_caps(const Bytes& text, Bytes& out)
{
    auto one = [&](const std::string& id, const std::vector<std::string>& cls, Bytes& e) {
        e = text;
        return replace_one(e, lp(kText), lp(id)) && set_classes(e, cls);
    };
    Bytes kids;
    for (int k = 0; k < 7; ++k) {
        std::string c = "om-hk-c" + std::to_string(k);
        std::vector<std::string> cls = k % 2 ? std::vector<std::string>{"om-hk-plus", c, "font-text-caption-white", "!cpp-none"}
                                             : std::vector<std::string>{k == 6 ? "om-hk-key" : "om-hk-mod", c, "!cpp-none"};
        Bytes e;
        if (!one("OmHkCap" + std::to_string(k), cls, e)) return false;
        kids.insert(kids.end(), e.begin(), e.end());
    }
    return one("OmHkCaps", {"om-hk-caps", "!cpp-none"}, out) && give_children(out, kids, 7);
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
        Bytes caps; // (the hotkey's key caps, after the value text)
        if (!make_caps(Bytes(res.begin() + cs, res.begin() + ce), caps)) return r.error = "could not make the key caps", false;
        copy.insert(copy.end(), caps.begin(), caps.end());
        std::uint32_t n;
        if (!u32(res, count_at, n) || n != 1) return r.error = "the wrap's child count isn't 1", false;
        res.insert(res.begin() + ce, copy.begin(), copy.end());
        put32(res, count_at, n + 2);
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

// Every occurrence of `old` in b replaced (the count of them).
int replace_all(Bytes& b, const Bytes& old, const Bytes& with)
{
    int n = 0;
    for (size_t at = find(b, old); at != SIZE_MAX; at = find(b, old, at + with.size())) {
        b.erase(b.begin() + at, b.begin() + at + old.size());
        b.insert(b.begin() + at, with.begin(), with.end());
        ++n;
    }
    return n;
}

// The quick peek (statusgaugeview2_game): outfits on an arc beside the character. Its elements go into the HP tracker
// (HPGaugeTracker: follows the character; the game never shows or hides it, so showing it while the peek is open
// fights nothing; in the stamina tracker, which the game hides while stamina is full, the arc flickered), in a scale
// layer like the stamina tracker's own (same classes: same place and size as the stamina gauge's):
//   OmPeekLayer (om-peek-layer ui-scale-center-mid ui-scale-stamina-gauge-bar cpp-ui-hud-scale-layer)
//   > OmPeekWrap (om-peek-wrap, "!cpp-none": hidden until the plugin shows it): a copy of PlayerTemperatureMarkWrap (a
//     plain element with the game's default script, shown by code), its child replaced by:
//     OmPeekArc (the arc art), OmPeekFrame0-4 (slot frames), OmPeekIcon0-4 (the outfits' chest icons, set by the
//     plugin), OmPeekLabel / OmPeekName (texts set by the plugin; the game's caption font with its outline)
// Every child is made from PlayerTemperatureMark, the plainest element there is (a class list, no children, no
// attributes, an id, empty text): only its classes and id change. Layout: tools/peek_gen.py (CSS merge).
constexpr SV kPeekSrc = "PlayerTemperatureMarkWrap", kPeekSrcBox = "PlayerTemperatureMark";
constexpr SV kPeekWrap = "OmPeekWrap", kPeekLayer = "OmPeekLayer", kPeekHome = "HPGaugeTracker";

// The class list at the start of element bytes `e` (flag 1, count, strings) replaced; false if it has none.
bool set_classes(Bytes& e, const std::vector<std::string>& cls)
{
    std::uint32_t n;
    if (e.empty() || e[0] != 1 || !u32(e, 1, n) || n > 64) return false;
    size_t i = 5;
    for (std::uint32_t k = 0; k < n; ++k)
        if (!str(e, i)) return false;
    Bytes head{1, 0, 0, 0, 0};
    put32(head, 1, std::uint32_t(cls.size()));
    for (const std::string& c : cls) {
        Bytes l = lp(c);
        head.insert(head.end(), l.begin(), l.end());
    }
    e.erase(e.begin(), e.begin() + i);
    e.insert(e.begin(), head.begin(), head.end());
    return true;
}

// A plain element like `tmpl` (PlayerTemperatureMark's bytes) with its own id and classes.
bool plain(const Bytes& tmpl, SV id, const std::vector<std::string>& cls, Bytes& out)
{
    out = tmpl;
    if (!set_classes(out, cls) || replace_all(out, lp(kPeekSrcBox), lp(id)) != 1) return false;
    std::vector<Found> f;
    size_t e;
    return element(out, 0, e, &f) && e == out.size() && f.size() == 1 && f[0].id == id;
}

// `child` as the last direct child of the element whose id is `parent` (its child count goes up); false if that
// element can't be parsed or has no children.
bool insert_last(Bytes& res, SV parent, const Bytes& child)
{
    size_t hs, he;
    std::vector<Found> home;
    size_t at = find(res, lp(parent));
    if (at == SIZE_MAX || !element_of(res, at, parent, hs, he, home)) return false;
    size_t last = 0, count_at = SIZE_MAX;
    for (const Found& f : home)
        if (f.child && f.parent_start == hs) last = std::max(last, f.end), count_at = f.parent_count;
    std::uint32_t n;
    if (count_at == SIZE_MAX || !u32(res, count_at, n) || n > 64) return false;
    res.insert(res.begin() + last, child.begin(), child.end());
    put32(res, count_at, n + 1);
    std::vector<Found> check;
    size_t e = 0;
    bool has = false;
    if (element(res, hs, e, &check))
        for (const Found& f : check) has |= f.start == last && f.child;
    return e == he + child.size() && has;
}

bool add_peek(Bytes& res, Report& r)
{
    size_t ws, we, bs, be;
    std::vector<Found> found, box;
    size_t at = find(res, lp(kPeekSrc));
    if (at == SIZE_MAX || !element_of(res, at, kPeekSrc, ws, we, found)) return r.error = "no PlayerTemperatureMarkWrap", false;
    at = find(res, lp(kPeekSrcBox));
    if (at == SIZE_MAX || !element_of(res, at, kPeekSrcBox, bs, be, box) || box.size() != 1)
        return r.error = "PlayerTemperatureMark isn't a plain element", false;
    Bytes tmpl(res.begin() + bs, res.begin() + be);
    // the children
    std::vector<std::pair<std::string, std::vector<std::string>>> kids = {{"OmPeekArc", {"om-peek-arc"}}};
    kids.push_back({"OmPeekLift", {"om-peek-lift"}}); // (#32: behind the middle slot while an outfit is picked up)
    for (int k = 0; k < 5; ++k) kids.push_back({"OmPeekFrame" + std::to_string(k), {"om-peek-frame", "om-peek-frame-" + std::to_string(k)}});
    for (int k = 0; k < 5; ++k) kids.push_back({"OmPeekIcon" + std::to_string(k), {"om-peek-icon", "om-peek-icon-" + std::to_string(k)}});
    kids.push_back({"OmPeekLabel", {"om-peek-label", "font-stroke-common", "font-text-caption-minor-white"}});
    kids.push_back({"OmPeekName", {"om-peek-name", "font-stroke-common", "font-text-caption-minor-white"}});
    // the badges (2026-11-06): each other slot's climate thermometer, the highlighted outfit's beside its label
    for (int k = 0; k < 5; ++k) kids.push_back({"OmPeekBadge" + std::to_string(k), {"om-peek-badge", "om-peek-badge-" + std::to_string(k), "!cpp-none"}});
    for (int k = 0; k < 2; ++k) kids.push_back({"OmPeekNb" + std::to_string(k), {"om-peek-nb", "om-peek-nb-" + std::to_string(k), "!cpp-none"}});
    // #32 pins (2026-11-13): each slot's corner mark (a pin / unpin to come), then the hint row (the button, what it does)
    for (int k = 0; k < 5; ++k) kids.push_back({"OmPeekMark" + std::to_string(k), {"om-peek-mark", "om-peek-mark-" + std::to_string(k), "!cpp-none"}});
    kids.push_back({"OmPeekHold", {"om-peek-hold"}}); // (behind the button: the game's hold look)
    kids.push_back({"OmPeekRipple", {"om-peek-ripple"}}); // (behind the button: the press's ripple)
    kids.push_back({"OmPeekHintIcon", {"om-peek-hint-icon", "!cpp-none"}});
    kids.push_back({"OmPeekHint", {"om-peek-hint", "font-stroke-common", "font-text-caption-minor-white", "!cpp-none"}});
    Bytes body;
    for (const auto& [id, cls] : kids) {
        Bytes e;
        if (!plain(tmpl, id, cls, e)) return r.error = "could not make " + id, false;
        body.insert(body.end(), e.begin(), e.end());
    }
    // the highlighted outfit's hotkey under its name: the gear wheel's key cap row (om-hk-caps, om-hk-c0..c6)
    {
        Bytes caps, capkids;
        for (int k = 0; k < 7; ++k) {
            std::string c = "om-hk-c" + std::to_string(k);
            std::vector<std::string> cls = k % 2 ? std::vector<std::string>{"om-hk-plus", c, "font-stroke-common", "font-text-caption-minor-white", "!cpp-none"}
                                                 : std::vector<std::string>{k == 6 ? "om-hk-key" : "om-hk-mod", c, "!cpp-none"};
            Bytes e;
            if (!plain(tmpl, "OmPeekHkC" + std::to_string(k), cls, e)) return r.error = "could not make the hotkey caps", false;
            capkids.insert(capkids.end(), e.begin(), e.end());
        }
        if (!plain(tmpl, "OmPeekHk", {"om-hk-caps", "om-peek-hk", "!cpp-none"}, caps) || !give_children(caps, capkids, 7))
            return r.error = "could not make the hotkey row", false;
        body.insert(body.end(), caps.begin(), caps.end());
    }
    const std::uint32_t n_body = std::uint32_t(kids.size()) + 1; // (+ the hotkey row)
    // the wrap: its one child replaced by them, its count set, its id and classes its own
    Bytes wrap(res.begin() + ws, res.begin() + we);
    const Found* child = nullptr;
    for (const Found& f : found)
        if (f.child && f.parent_start == ws) child = &f;
    if (!child || child->id != kPeekSrcBox) return r.error = "PlayerTemperatureMarkWrap's child changed", false;
    size_t cs = child->start - ws, ce = child->end - ws, count_at = child->parent_count - ws;
    wrap.erase(wrap.begin() + cs, wrap.begin() + ce);
    wrap.insert(wrap.begin() + cs, body.begin(), body.end());
    put32(wrap, count_at, n_body);
    if (replace_all(wrap, lp(kPeekSrc), lp(kPeekWrap)) != 1) return r.error = "the wrap's id isn't there once", false;
    replace_all(wrap, lp("#PlayerTemperatureMarkWrap"), lp("#OmPeekWrap")); // (its selector-show-layer-list)
    if (!set_classes(wrap, {"om-peek-wrap", "!cpp-none"})) return r.error = "the wrap has no class list", false;
    std::vector<Found> check;
    size_t e;
    int n_kids = 0;
    if (element(wrap, 0, e, &check))
        for (const Found& f : check) n_kids += f.child && f.parent_start == 0;
    if (e != wrap.size() || check.empty() || check.back().id != kPeekWrap || n_kids != int(n_body))
        return r.error = "the peek wrap doesn't parse", false;
    // the layer: a plain element with the wrap as its one child (children flag after the class list: 0 -> 1, count)
    Bytes layer;
    if (!plain(tmpl, kPeekLayer, {"om-peek-layer", "ui-scale-center-mid", "ui-scale-stamina-gauge-bar", "cpp-ui-hud-scale-layer"}, layer))
        return r.error = "could not make the peek layer", false;
    {
        size_t i = 5;
        std::uint32_t nc = 0;
        u32(layer, 1, nc);
        for (std::uint32_t k = 0; k < nc; ++k) str(layer, i);
        if (layer[i] != 0) return r.error = "the plain element has children", false;
        layer[i] = 1;
        Bytes cnt{1, 0, 0, 0};
        layer.insert(layer.begin() + i + 1, cnt.begin(), cnt.end());
        layer.insert(layer.begin() + i + 5, wrap.begin(), wrap.end());
        check.clear();
        if (!element(layer, 0, e, &check) || e != layer.size() || check.back().id != kPeekLayer)
            return r.error = "the peek layer doesn't parse", false;
    }
    // the home: the HP tracker; the layer goes after its last child (its own gauge, hidden by the plugin while shown)
    if (!insert_last(res, kPeekHome, layer)) return r.error = "no HPGaugeTracker", false;
    ++r.peeks;
    return true;
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
    if (name == "statusgaugeview2_game.uibinary") return 4;
    return 0;
}

bool patch(const Bytes& in, unsigned what, Bytes& out, Report& r)
{
    out.clear();
    r = Report{};
    if (((what & 3) && (count(in, lp(kNew)) || count(in, lp(kValue)))) || ((what & 4) && count(in, lp(kPeekWrap))))
        return r.error = "already has the additions", false;
    Bytes res = in;
    if ((what & 1) && (!add_headers(res, r) || !r.headers)) {
        if (r.error.empty()) r.error = "no QuickSlotHeader8";
        return false;
    }
    if ((what & 2) && (!add_values(res, r) || !r.values)) {
        if (r.error.empty()) r.error = "no CoolTimeChargedCountWrap";
        return false;
    }
    if ((what & 4) && !add_peek(res, r)) return false;
    out.swap(res);
    return true;
}

}
