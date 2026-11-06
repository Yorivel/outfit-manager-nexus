// UI element helpers: classes, layout checks, labels, images, dumps, messages.
#include "om.h"
#include "ui.h"
#include "addrs.h"
#include "items.h"
#include "settings.h"
#include "om_rest.h"

namespace om {

// --- UI elements --------------------------------------------------------------------------------
// Element: +0x20 class holder {element*, vector<u32 classId>*}. Script object: +8 element.
std::string class_name(U32 id)
{
    U64 tbl = rp(g_base + kRvaClassNames);
    U64 arr = tbl ? rp(tbl + 0x58) : 0;
    std::string n = arr ? cstr(rp(arr + U64(id) * 0x10)) : std::string();
    if (n.empty()) {
        char b[16];
        std::snprintf(b, sizeof b, "#%u", id);
        n = b;
    }
    return n;
}

bool is_element(U64 e)
{
    U64 holder = rp(e + 0x20);
    return holder && rp(holder) == e;
}

std::string classes(U64 elem)
{
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    U64 data = vec ? rp(vec) : 0;
    U32 n = vec ? r32(vec + 8) : 0;
    if (!data || n > 64) return "(no classes)";
    std::string s;
    for (U32 i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += class_name(r32(data + i * 4ull));
    }
    return s;
}

// Describes a pointer that is either an element or a script object owning one.
std::string describe(U64 p)
{
    char b[64];
    if (is_element(p)) {
        std::snprintf(b, sizeof b, "elem %llX flags b0=%02X b1=%02X: ", p, r32(p + 0xb8) & 0xFF, r32(p + 0xb9) & 0xFF);
        return b + classes(p);
    }
    U64 e = rp(p + 8);
    if (e && is_element(e)) {
        std::snprintf(b, sizeof b, "script %llX vt=+%llX elem %llX b0=%02X: ", p, rp(p) - g_base, e,
                      r32(e + 0xb0) & 0xFF);
        return b + classes(e);
    }
    std::snprintf(b, sizeof b, "? %llX: ", p);
    return b + hex(p, 0x20);
}

// --- Dumps --------------------------------------------------------------------------------------
std::string str_id(U64 at)
{
    U16 id = 0xFFFF;
    if (!rd(at, id)) return "?";
    U64 si = g_str_get(0, id);
    std::string s = si ? cstr(rp(rp(si + 0x10))) : std::string();
    char b[16];
    std::snprintf(b, sizeof b, "[%u]", id);
    return b + s;
}

void dump_quickslotinfo()
{
    U64 mgr = rp(g_base + kRvaQuickSlotInfoMgr);
    U32 count = mgr ? r32(mgr + 8) : 0;
    LOG("QuickSlotInfo mgr %llX: %u records", mgr, count);
    for (U32 i = 0; i < count && i < 16; ++i) {
        U16 key = static_cast<U16>(i);
        U64 rec = g_qsi_get(0, key);
        if (!rec) continue;
        U32 slots = r32(rec + kRecSlotCount);
        U64 sarr = rp(rec + kRecSlots);
        LOG("  rec %u @%llX slots=%u head: %s", i, rec, slots, hex(rec, 0x40).c_str());
        for (U32 s = 0; s < slots && s < 16 && sarr; ++s) {
            U64 so = sarr + s * 0x50ull;
            LOG("    slot %u widget=%s comp=%s raw: %s", s, str_id(so + 2).c_str(), str_id(so + 4).c_str(),
                hex(so, 0x50).c_str());
        }
    }
}

void dump_menu(U64 self, const char* why)
{
    LOG("---- RootQuickSlot %llX: %s", self, why);
    U64 tb = rp(self + kOffRootTabBar);
    if (tb) {
        U32 n = r32(tb + kOffTabHeaderCount);
        LOG("tabbar %llX headers=%u prev=%d cur=%d sel=%d default=%d flags120=%s", tb, n, r32(tb + 0x110),
            r32(tb + kOffTabCurrent), r32(tb + 0x11c), r32(tb + 0x118), hex(tb + 0x120, 8).c_str());
        U64 hv = rp(tb + kOffTabHeaders);
        for (U32 i = 0; i < n && i < 16 && hv; ++i) {
            U64 h = rp(hv + i * 8ull);
            LOG("  header %u e0=%u %s", i, r32(h + 0xe0) & 0xFF, describe(h).c_str());
        }
    }
    U32 bodies = r32(self + kOffRootBodyCount);
    U64 bv = rp(self + kOffRootBodies);
    LOG("bodies=%u  +230(defTab)=%d +284=%04X +286=%04X +288=%d +28c..291=%s", bodies, r32(self + 0x230),
        r32(self + 0x284) & 0xFFFF, r32(self + 0x286) & 0xFFFF, r32(self + 0x288), hex(self + 0x28c, 6).c_str());
    for (U32 i = 0; i < bodies && i < 16 && bv; ++i)
        LOG("  body %u %s", i, describe(rp(bv + i * 8ull)).c_str());
    U64 sv = rp(self + kOffRootSlotVecs);
    U32 tabs = r32(self + kOffRootSlotVecCount);
    for (U32 i = 0; i < tabs && i < 16 && sv; ++i) {
        U32 cnt = r32(sv + i * 0x10ull + 8);
        U64 arr = rp(sv + i * 0x10ull);
        LOG("  tab %u slot scripts=%u first: %s", i, cnt, arr && cnt ? describe(rp(arr)).c_str() : "-");
    }
    for (U64 off : {0x1a0ull, 0x1a8ull, 0x1b0ull, 0x1c0ull, 0x1d0ull, 0x1b8ull}) {
        U64 p = rp(self + off);
        LOG("  +%llX: %s", off, p ? describe(p).c_str() : "null");
    }
    LOG("  elem: %s", describe(self).c_str());
}

// --- Type checks --------------------------------------------------------------------------------
// Every object reached through a struct offset must have its class. A pointer read at a moved offset practically
// never has the expected vtable, so a failed check means the game's layout changed in a way the offsets did not
// follow: the mod then stays off (no Outfits tab) and says so once.
bool has_vt(U64 obj, U64 vt_rva) { return vt_rva && obj && rp(obj) == g_base + vt_rva; }

std::string g_layout_bad; // what failed ("" = fine)
bool g_layout_toast_done = false;

// At build time (the bodies are about to be made): the menu and its tab bar.
bool layout_ok_build(U64 self)
{
    U64 tb = rp(self + kOffRootTabBar);
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    const char* why = !has_vt(self, kRvaRootQuickSlotVt) ? "menu"
                      : !has_vt(tb, kRvaTabBarVt)       ? "tab bar"
                      : !n || n > 16 || !has_vt(rp(rp(tb + kOffTabHeaders)), kRvaHeaderVt) ? "tab headers"
                                                                                         : nullptr;
    if (why && g_layout_bad.empty()) {
        g_layout_bad = why;
        LOG("layout check failed at build: %s (menu %llX, tab bar %llX, %u headers); mod off", why, self, tb, n);
    }
    return !why;
}

// When the menu opens (everything built): also the centre tooltip, cursor, bodies and the Armor slot scripts.
bool layout_ok(U64 self)
{
    static U64 checked = 0;
    if (!g_layout_bad.empty()) return false;
    if (checked == self) return true;
    if (!layout_ok_build(self)) return false;
    U64 bodies = rp(self + kOffRootBodies), sv = rp(self + kOffRootSlotVecs);
    U32 nb = r32(self + kOffRootBodyCount), ns = r32(self + kOffRootSlotVecCount);
    const char* why = !has_vt(rp(self + kOffRootCenterTip), kRvaCenterTipVt) ? "centre tooltip"
                      : !has_vt(rp(self + kOffRootCursor), kRvaRadialCursorVt) ? "radial cursor"
                      : !nb || nb > 16 || !has_vt(rp(bodies), kRvaBodyVt)   ? "tab bodies"
                      : ns <= kTemplateRecord || ns > 16 ||
                              !has_vt(rp(rp(sv + kTemplateRecord * 0x10ull)), kRvaEquipSlotVt) ? "armor slots"
                                                                                               : nullptr;
    if (why) {
        g_layout_bad = why;
        LOG("layout check failed: %s (bodies %u, slot tabs %u); mod off", why, nb, ns);
        return false;
    }
    checked = self;
    LOG("layout check passed (menu %llX)", self);
    return true;
}

U64 __fastcall build_hook(U64 self, U64 out, U64 a, U64 b)
{
    U64 tables[2] = {qsi_table(kRvaQsiTableRoot0), qsi_table(kRvaQsiTableRoot1)};
    U64 tb = rp(self + kOffRootTabBar);
    U32 headers = tb ? r32(tb + kOffTabHeaderCount) : 0;
    U32 records = tables[0] ? r32(tables[0] + 8) : 0;
    LOG("build bodies: self %llX tabbar %llX headers=%u records=%u tables %llX %llX", self, tb, headers, records,
        tables[0], tables[1]);

    // Only inject when the thtml has exactly one extra header (= the browser mod is active).
    bool inject = records == 4 && headers == records + 1 && layout_ok_build(self) && prepare_fake_record();
    if (!inject) {
        LOG("  no Outfits tab injected%s", g_ui_tab_added ? "" : " (the gear wheel's UI files got no Outfits header at load: see \"gear wheel UI\" above)");
        g_fake_key = 0xFFFF; // (an earlier build's record must not be served for this one)
        return g_build_orig(self, out, a, b);
    }
    g_fake_key = records;
    g_outfits_root = self; // (only this menu has the Outfits tab: every other one stays the game's)
    U32 bumped[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (!tables[i] || (i == 1 && tables[1] == tables[0])) continue;
        U32* cnt = reinterpret_cast<U32*>(tables[i] + 8);
        if (*cnt != records) continue;
        *cnt = records + 1;
        bumped[i] = 1;
    }
    U64 r = g_build_orig(self, out, a, b);
    for (int i = 0; i < 2; ++i)
        if (bumped[i]) *reinterpret_cast<U32*>(tables[i] + 8) = records;
    LOG("  injected record %u; result %u, bodies now %u", records, r32(out), r32(self + kOffRootBodyCount));
    return r;
}

// Depth-first search for the first element below `elem` carrying class `cls`.
U64 find_by_class(U64 elem, const char* cls, int max_depth, int depth)
{
    if (!elem || depth > max_depth) return 0;
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    U64 data = vec ? rp(vec) : 0;
    U32 n = vec ? r32(vec + 8) : 0;
    for (U32 i = 0; data && i < n && i < 64; ++i)
        if (class_name(r32(data + i * 4ull)) == cls) return elem;
    U64 kids = rp(elem + 0x40);
    U32 count = r32(elem + 0x48);
    for (U32 i = 0; kids && i < count && i < 64; ++i)
        if (U64 hit = find_by_class(rp(kids + i * 8ull), cls, max_depth, depth + 1)) return hit;
    return 0;
}

// The element whose child has class `cls` (for elements with no class of their own).
U64 find_parent_of_class(U64 elem, const char* cls, int max_depth, int depth)
{
    if (!elem || depth > max_depth) return 0;
    U64 kids = rp(elem + 0x40);
    U32 count = r32(elem + 0x48);
    for (U32 i = 0; kids && i < count && i < 64; ++i) {
        U64 k = rp(kids + i * 8ull);
        if (k && find_by_class(k, cls, 0) == k) return elem;
        if (U64 hit = find_parent_of_class(k, cls, max_depth, depth + 1)) return hit;
    }
    return 0;
}

bool g_label_logged = false;

// The header script's "show" virtual (+0x20), only on a tab header (CheckBox script), SEH-guarded.
void show_header(U64 h)
{
    U64 fn = has_vt(h, kRvaHeaderVt) ? rp(rp(h) + 0x20) : 0;
    if (!fn) {
        LOG("Outfits header %llX is not a tab header script; not shown", h);
        return;
    }
    __try {
        reinterpret_cast<VoidFn>(fn)(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("Outfits header: show faulted");
    }
}

// Header of the Outfits tab, or 0 if the tab was not injected.
U64 outfits_header(U64 self)
{
    U32 idx = g_fake_key.load();
    if (idx == 0xFFFF || !layout_ok(self)) return 0;
    U64 tb = rp(self + kOffRootTabBar);
    U64 hv = tb ? rp(tb + kOffTabHeaders) : 0;
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    return hv && n > idx ? rp(hv + idx * 8ull) : 0;
}

// The header has no localstring in the thtml; its text is set here.
void set_label(U64 h)
{
    U64 text = find_by_class(rp(h + 8), "quickslot-tabbar-header-text");
    if (text) g_set_text(text, lang::t("tab.outfits"), 0);
    if (!g_label_logged) {
        g_label_logged = true;
        LOG("Outfits header %s, label elem %llX", describe(h).c_str(), text);
    }
}

// Adds or removes a CSS class the way the game's scripts do (skipped while the element is mid-restyle).
bool set_class(U64 elem, const char* cls, bool on)
{
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    if (!vec) return false;
    U64 pending = rp(rp(holder) + 0xa0);
    if (pending && rp(pending + 8)) return false;
    U64 id = 0;
    g_intern(&id, cls, 1, 0x2ffff);
    U32 cid = static_cast<U32>(id);
    if (!cid) return false;
    if (on) {
        if (!g_vec_add(vec, &cid)) return true; // already set
        g_vec_sort(vec);
    } else if (g_vec_remove(vec, &cid) == 0xFFFFFFFF) {
        return true; // not set
    }
    g_restyle(holder);
    return true;
}

// Replaces the numbered class previously applied to `elem` (tracked in `applied`) with `want` ("" = none).
bool swap_class(U64 elem, std::string& applied, const std::string& want)
{
    if (applied == want) return true;
    if (!applied.empty()) {
        if (!set_class(elem, applied.c_str(), false)) return false;
        applied.clear();
    }
    if (!want.empty()) {
        if (!set_class(elem, want.c_str(), true)) return false;
        applied = want;
    }
    return true;
}

bool has_class(U64 elem, const char* cls) { return elem && find_by_class(elem, cls, 0) == elem; }

// Sets an element's background image to a texture by name, the way item icons get theirs (f35fb0).
// 1 = set, 0 = no such texture, -1 = fault.
int set_image_raw(U64 self, U64 elem, const char* name)
{
    alignas(16) U64 var[2] = {g_base + kRvaEmptyString, 2};
    int r = -1;
    __try {
        g_str_assign(var, name);
        g_str_normalize(var);
        g_str_lower(var);
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        if (!g_texture_exists(*reinterpret_cast<U64*>(ui + kOffUiTextures), var)) {
            r = 0;
        } else {
            U64 prop = kPropBackgroundImage, flag = 0;
            g_set_style_prop(*reinterpret_cast<U64*>(elem + 0x20), &prop, var, &flag);
            r = 1;
        }
        g_str_destroy(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    return r;
}
SlotImage g_slot_image[kOutfitSlots];
bool g_rehide_logged = false;

// Shows text in the game's own toast notification (and the log).
void notify(U64 self, const std::string& text)
{
    LOG("notify: %s", text.c_str());
    if (!g_toast) return;
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        if (ui) g_toast(ui, text.c_str(), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("notify: toast faulted");
    }
}

// A report after the fact (pieces left, not taken, overwritten): silenced by "Messages: Off" (still logged).
void report(U64 self, const std::string& text)
{
    if (g_messages) notify(self, text);
    else LOG("(message off) %s", text.c_str());
}

} // namespace om
