// The game's dialogs: the rename prompt, typed characters, yes / no questions.
#include "om.h"
#include "prompts.h"
#include "addrs.h"
#include "ui.h"
#include "store.h"
#include "load.h"
#include "actions.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "input.h"

namespace om {

// Vtable slots: the known build's numbers; adopt_slots() takes the slot each function is in now (resolver).
int kSlotTextModalClick = 115;   // (modal, ?, clicked element, char): +0xd8 confirm, +0xe0 cancel
int kSlotCommonModalClick = 115; // same handler role in the CommonModal
constexpr U32 kParamsSize = 0x1428; // (0x1310 before the 2026-10-30 update: one more label buffer at the end)
constexpr U32 kParamsMessage = 0x118, kParamsMessageCap = 0x801, kParamsMessageLen = 0x92c;
constexpr int kNameMaxChars = 24;
constexpr U32 kParamsTitle = 0x0, kParamsTitleLen = 0x114, kParamsConfirm = 0xfc0, kParamsCancel = 0x10d8;
constexpr U32 kParamsLabelCap = 0x100; // inline buffers of 0x108 bytes
VoidFn g_hide_script = nullptr;
ModalClickFn g_text_click_orig = nullptr;
ModalClickFn g_ask_click_orig = nullptr;

// --- Rename prompt: the game's own TextEditModalMessage (as its naming prompts open it) ----------
// Confirm with text renames the outfit just saved; an empty confirm, cancel, Esc or closing the menu keep "Outfit N".
std::atomic<U64> g_name_modal{0}; // the open prompt (0: none)
U64 g_name_self = 0;       // the gear menu it was opened from
std::string g_name_default;
size_t g_name_index = 0;   // the outfit being named (checked against g_name_default)
U64 g_name_opened = 0;     // tick
U64 g_modal_ended = 0;      // tick: presses made while typing are dropped for a moment after the prompt closes
unsigned char g_name_shown = 0; // modal +0xbd at open (the shown flag; the hide clears it)
alignas(16) unsigned char g_modal_params[0x1800];
static_assert(sizeof(g_modal_params) >= kParamsSize, "the modal parameter block does not fit");

// Object-free (for __try): the opened modal or 0.
U64 open_text_modal_raw(U64 self, const char* message)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        g_params_init(reinterpret_cast<U64>(g_modal_params));
        strncpy_s(reinterpret_cast<char*>(g_modal_params + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(g_modal_params + kParamsMessageLen) = 0xFFFFFFFF; // length: recompute
        U64 modal = g_open_text_modal(0, view, g_modal_params, kNameMaxChars);
        g_params_free(reinterpret_cast<U64>(g_modal_params));
        if (modal && !has_vt(modal, kRvaTextModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback (a stale one from earlier use)
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Object-free: the prompt's current text (empty if it can't be read).
void read_modal_text_raw(U64 modal, char* out, int size)
{
    out[0] = 0;
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (input && *reinterpret_cast<unsigned char*>(input + kOffInputBusy) == 0)
            g_get_text(*reinterpret_cast<U64*>(input + kOffInputText), out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

// Object-free: closes the prompt the way its own buttons do.
void hide_modal_raw(U64 modal)
{
    __try {
        g_hide_script(modal);
        if (kOffModalD0)
            if (U64 d0 = *reinterpret_cast<U64*>(modal + kOffModalD0)) *reinterpret_cast<unsigned char*>(d0 + 0xf2) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: hide faulted");
    }
}

U64 call_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_text_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: game click handler faulted");
        return 0;
    }
}

bool name_invalid_raw(const char* s)
{
    __try {
        return g_name_invalid(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string clean_name(const std::string& in)
{
    std::string s;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) s += c; // no tabs / newlines (store format)
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
QueueHideFn g_queue_hide = nullptr;
bool g_release_pending = false; // set when a prompt closes while the menu is open; done by pump on the next frame

// The input side of the same problem: the HUD key binding that holds the menu (0xcc40c0's binding: +0x232 "pressed")
// never sees its key go up either, so the next press only counts as the missing release. The hook remembers the binding
// whose down / held event left it pressed (with a copy of that event); after a prompt the game is handed that event
// again as an up (type 7 at +0x38) and runs its own release: binding state, hold progress, the HUD's release handler
// (which closes the menu).
using BindingInputFn = void(__fastcall*)(U64, U64, U64);
BindingInputFn g_binding_input_orig = nullptr;
constexpr U32 kInputEventCopy = 0x40; // the fields the state machine reads end at +0x3a
struct HeldBinding {
    U64 dispatcher = 0, binding = 0;
    alignas(16) unsigned char event[kInputEventCopy] = {};
};
std::map<U64, HeldBinding> g_pressed; // bindings whose key is down, by binding
U64 g_last_pressed = 0;               // the binding pressed last (at menu open: the one that opened it)
HeldBinding g_held;                   // the menu's binding, taken at menu open
std::mutex g_held_mu; // input may arrive on another thread than the menu's

// At menu open: the binding pressed last is the menu's button.
void note_menu_binding()
{
    std::lock_guard lock(g_held_mu);
    auto it = g_pressed.find(g_last_pressed);
    g_held = it != g_pressed.end() ? it->second : HeldBinding{};
}

// 1 = pressed by a down / held event, 2 = released (up), 0 = neither or unreadable.
int binding_change(U64 event, U64 binding, unsigned char* copy)
{
    __try {
        unsigned char type = *reinterpret_cast<unsigned char*>(event + kOffEventType);
        if ((type == 3 || type == 6) && *reinterpret_cast<unsigned char*>(binding + kOffBindingPressed)) {
            std::memcpy(copy, reinterpret_cast<const void*>(event), kInputEventCopy);
            return 1;
        }
        return type == 7 ? 2 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall binding_input_hook(U64 dispatcher, U64 event, U64 binding)
{
    g_binding_input_orig(dispatcher, event, binding);
    if (!kOffEventType || !kOffBindingPressed) return; // (offsets doubtful after a game update)
    alignas(16) unsigned char copy[kInputEventCopy];
    int change = binding_change(event, binding, copy);
    if (!change) return;
    std::lock_guard lock(g_held_mu);
    if (change == 1) {
        HeldBinding& h = g_pressed[binding];
        h.dispatcher = dispatcher, h.binding = binding;
        std::memcpy(h.event, copy, kInputEventCopy);
        g_last_pressed = binding;
    } else {
        g_pressed.erase(binding);
        if (binding == g_held.binding) g_held = {};
    }
}

// 1 = released through the game's input, 0 = its up did arrive, -1 = fault.
int send_up(HeldBinding& h)
{
    __try {
        if (!*reinterpret_cast<unsigned char*>(h.binding + kOffBindingPressed)) return 0;
        if (!kOffEventType || kOffEventType >= kInputEventCopy) return -1; // layout doubtful / beyond our copy
        h.event[kOffEventType] = 7; // key up
        h.event[0x10] = 0; // "handled" (the state machine sets it)
        g_binding_input_orig(h.dispatcher, reinterpret_cast<U64>(h.event), h.binding);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// True if a held binding was released through the game's own input path.
bool release_held_binding()
{
    HeldBinding h;
    {
        std::lock_guard lock(g_held_mu);
        h = g_held;
        g_held = {};
        g_pressed.erase(h.binding);
    }
    if (!h.binding || !g_binding_input_orig) return false;
    int r = send_up(h);
    LOG("prompt closed: %s (binding %llX)", r > 0 ? "menu button released through the game's input" :
        r == 0 ? "menu button was already up" : "input release faulted", h.binding);
    return r > 0;
}

void release_menu_button(U64 self)
{
    if (release_held_binding()) return;
    if (!kOffPanelFlags || !kOffPanelHoldClose || !kOffPanelElem) return; // (doubtful after a game update)
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ItemQuickSlotView2") : 0;
        if (!view || (*reinterpret_cast<unsigned char*>(view + kOffViewFlags) & 0x60) != 0x40) return; // not shown
        U64 inner = *reinterpret_cast<U64*>(view + kOffViewInner);
        U64 panel = inner ? *reinterpret_cast<U64*>(inner + 0x10) : 0;
        if (!panel) return;
        auto flags = reinterpret_cast<unsigned char*>(panel + kOffPanelFlags);
        *flags &= 0xfe; // the button is up
        bool held_open = (*flags & 2) && *reinterpret_cast<unsigned char*>(panel + kOffPanelHoldClose); // opened by holding
        LOG("prompt closed: menu button released (%s)", held_open ? "menu closes" : "menu was opened by a tap: stays");
        if (held_open) {
            U64 elem = *reinterpret_cast<U64*>(panel + kOffPanelElem);
            if (!is_element(elem)) {
                LOG("prompt closed: the menu's view element is not where expected; not closing it");
                return;
            }
            g_queue_hide(*reinterpret_cast<U64*>(elem + 0x58), elem);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("prompt closed: menu button release faulted");
    }
}

void finish_naming(const std::string& text, const char* why)
{
    if (!g_name_modal) return;
    Busy busy;
    g_name_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    U64 self = g_name_self;
    std::string name = clean_name(text);
    bool here = g_name_index < g_outfits.size() && g_outfits[g_name_index].name == g_name_default;
    LOG("rename: %s, text \"%s\" -> \"%s\"%s", why, text.c_str(), name.c_str(), here ? "" : " (outfit gone)");
    if (!here || name.empty() || name == g_name_default) return;
    for (const Outfit& o : g_outfits)
        if (o.name == name) {
            if (g_menu_open) notify(self, lang::f("toast.name_taken", {{"outfit", name}}));
            return;
        }
    g_outfits[g_name_index].name = name;
    save_outfits();
    if (g_menu_open && outfits_current(self)) {
        center_restore();
        center_for_focus(self, focused_script(self), true);
    }
}

// Slot 113 of the TextEdit modal (its button / key handler): only our prompt is handled here.
U64 __fastcall text_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_name_modal || modal != g_name_modal || (flag & 0xFF)) return g_text_click_orig(modal, a, elem, flag);
    if (elem && elem == rp(modal + kOffModalConfirm)) {
        char buf[0x105] = {};
        read_modal_text_raw(modal, buf, sizeof buf);
        std::string text = buf;
        if (clean_name(text).empty()) { // the game ignores an empty confirm; here it keeps the default name
            hide_modal_raw(modal);
            finish_naming("", "confirmed empty");
            return 0;
        }
        if (name_invalid_raw(buf)) {
            LOG("rename: \"%s\" rejected by the game's name filter", buf);
            return call_click_raw(modal, a, elem, flag); // shows the game's error and keeps the prompt open
        }
        U64 r = call_click_raw(modal, a, elem, flag); // validates again and closes
        finish_naming(text, "confirmed");
        return r;
    }
    if (elem && elem == rp(modal + kOffModalCancel)) {
        U64 r = call_click_raw(modal, a, elem, flag);
        finish_naming("", "cancelled");
        return r;
    }
    return g_text_click_orig(modal, a, elem, flag);
}
std::mutex g_typed_mu;
std::wstring g_typed;       // characters since the batch began
std::string g_typed_base;   // the box text on the frame before the batch
U32 g_typed_caret = 0;      // and its caret
int g_typed_quiet = 0;      // frames since the last character
std::string g_name_text;    // the box text on the last frame
U32 g_name_caret = 0;       // its caret

// Object-free: the prompt's caret (false if it can't be read, or a selection is open).
bool read_modal_caret_raw(U64 modal, U32& caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input) return false;
        caret = *reinterpret_cast<U32*>(input + kOffInputCaret);
        return caret == *reinterpret_cast<U32*>(input + kOffInputCaretEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Object-free: sets the prompt's text and caret (false if it can't be reached).
bool write_modal_text_raw(U64 modal, const char* text, U32 caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input || *reinterpret_cast<unsigned char*>(input + kOffInputBusy) != 0) return false;
        g_set_text(*reinterpret_cast<U64*>(input + kOffInputText), text, 0);
        *reinterpret_cast<U32*>(input + kOffInputCaret) = caret;
        *reinterpret_cast<U32*>(input + kOffInputCaretEnd) = caret;
        *reinterpret_cast<unsigned char*>(input + kOffInputCaretMoved) = 1; // as the game's own caret keys: redrawn now
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t utf8_chars(const std::string& s)
{
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// The byte offset of character `chars` (the end if past it).
size_t utf8_offset(const std::string& s, size_t chars)
{
    size_t i = 0;
    for (; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && chars-- == 0) break;
    return i;
}

void type_dropped_chars()
{
    char buf[0x105] = {};
    read_modal_text_raw(g_name_modal, buf, sizeof buf);
    U32 caret = 0;
    bool caret_ok = read_modal_caret_raw(g_name_modal, caret);
    std::string now = buf, base;
    std::wstring typed;
    U32 base_caret = 0;
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = now;
        g_name_caret = caret_ok ? caret : 0xFFFFFFFF;
        if (g_typed.empty() || ++g_typed_quiet < 2) return;
        typed.swap(g_typed);
        base = g_typed_base;
        base_caret = g_typed_caret;
    }
    // What the game typed went in at the caret: the text around it must be as before (else: deleted / a selection /
    // edited in the same moment, and it is left as it is).
    size_t pos = base_caret == 0xFFFFFFFF ? std::string::npos : utf8_offset(base, base_caret);
    size_t tail = pos == std::string::npos ? 0 : base.size() - pos;
    if (pos == std::string::npos || now.size() < base.size() || now.compare(0, pos, base, 0, pos) != 0 ||
        now.compare(now.size() - tail, tail, base, pos, tail) != 0) {
        LOG("rename: typed characters skipped (text \"%s\" was \"%s\", caret %u)", now.c_str(), base.c_str(), base_caret);
        return;
    }
    std::string out = now;
    bool added = false;
    for (wchar_t c : typed) {
        char u[8] = {};
        int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, nullptr, nullptr);
        if (n <= 0) continue;
        if (out.compare(pos, size_t(n), u, size_t(n)) == 0) { pos += size_t(n); continue; } // the game took it
        if (utf8_chars(out) >= size_t(kNameMaxChars)) break;
        out.insert(pos, u, size_t(n));
        pos += size_t(n);
        added = true;
    }
    if (!added) return;
    U32 at = U32(utf8_chars(out.substr(0, pos)));
    if (write_modal_text_raw(g_name_modal, out.c_str(), at)) {
        LOG("rename: typed characters added: \"%s\" -> \"%s\"", now.c_str(), out.c_str());
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = out;
        g_name_caret = at;
    }
}

void open_rename_prompt(U64 self, size_t index)
{
    if (!g_open_text_modal || g_name_modal) return;
    std::string msg = lang::f("prompt.name", {{"outfit", g_outfits[index].name}});
    U64 modal = open_text_modal_raw(self, msg.c_str());
    if (!modal) {
        LOG("rename: prompt did not open; name stays \"%s\"", g_outfits[index].name.c_str());
        return;
    }
    g_name_modal = modal;
    g_name_self = self;
    g_name_index = index;
    g_name_default = g_outfits[index].name;
    g_name_opened = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_typed.clear();
        g_name_text.clear();
        g_name_caret = 0;
    }
    watch_game_chars();
    rd(modal + kOffModalShown, g_name_shown);
    unsigned char hiding = 0;
    rd(modal + kOffModalHidden, hiding);
    LOG("rename: prompt %llX open for \"%s\" (+0xbd %u, +0xbe %u, input %llX)", modal, g_name_default.c_str(),
        g_name_shown, hiding, rp(modal + kOffModalInput));
}

// While the prompt is open: notices it closing some other way (the shown flag drops, or a 3-minute safety stop).
void watch_rename_prompt()
{
    if (!g_name_modal) return;
    type_dropped_chars();
    unsigned char shown = g_name_shown;
    rd(g_name_modal + kOffModalShown, shown);
    if (g_name_shown && !shown) finish_naming("", "prompt closed elsewhere");
    else if (GetTickCount64() - g_name_opened > 180000) {
        hide_modal_raw(g_name_modal);
        finish_naming("", "timed out");
    }
}
std::atomic<U64> g_ask_modal{0}; // the open question (0: none)
U64 g_ask_self = 0;
U64 g_ask_opened = 0;
unsigned char g_ask_shown = 0;
AskedLoad g_pending;
enum AskAnswer { kAskAbort, kAskYes, kAskNo };

U64 open_common_modal_raw(U64 self, const char* title, const char* message, const char* yes, const char* no)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        unsigned char* p = g_modal_params;
        g_params_init(reinterpret_cast<U64>(p));
        strncpy_s(reinterpret_cast<char*>(p + kParamsTitle), kParamsLabelCap, title, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsTitleLen) = 0xFFFFFFFF;
        strncpy_s(reinterpret_cast<char*>(p + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsMessageLen) = 0xFFFFFFFF;
        // (an empty label hides its button)
        strncpy_s(reinterpret_cast<char*>(p + kParamsConfirm), kParamsLabelCap, yes, _TRUNCATE);
        strncpy_s(reinterpret_cast<char*>(p + kParamsCancel), kParamsLabelCap, no, _TRUNCATE);
        U64 modal = g_open_common_modal(0, view, p, reinterpret_cast<const char*>(g_base + kRvaCommonModalName));
        g_params_free(reinterpret_cast<U64>(p));
        if (modal && !has_vt(modal, kRvaCommonModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

U64 call_ask_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_ask_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("missing pieces: game click handler faulted");
        return 0;
    }
}

bool g_ask_next = false; // the next question of g_pending opens on the next pump (not inside a modal's click)

void finish_ask(int answer, const char* why)
{
    if (!g_ask_modal) return;
    g_ask_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    AskedLoad p = std::move(g_pending);
    g_pending = {};
    bool here = p.index < g_outfits.size() && g_outfits[p.index].name == p.name;
    LOG("question %d: %s (\"%s\"%s)", p.stage, why, p.name.c_str(), here ? "" : ", outfit gone");
    if (p.confirm == kConfirmCleanSave || p.confirm == kConfirmUninstall) { // (about no one outfit)
        if (answer != kAskYes || !g_menu_open) return;
        if (p.confirm == kConfirmCleanSave) clean_outfits(g_ask_self);
        else uninstall(g_ask_self);
        return;
    }
    bool from_wheel = g_ask_self == g_outfits_root; // (or the quick peek: asked in the game, no wheel to close)
    if (!here || answer == kAskAbort || (from_wheel && !g_menu_open)) return;
    if (p.confirm) {
        bool other_here = p.confirm != kConfirmSwap ||
                          (p.other < g_outfits.size() && g_outfits[p.other].name == p.other_name);
        if (answer == kAskYes && other_here) confirmed(g_ask_self, p.index, p.confirm, p.other);
        return;
    }
    if (p.stage == 1) p.takes_answered = true, p.take = answer == kAskYes;
    else p.copies_answered = true, p.use_copies = answer == kAskYes;
    if (p.stage == 1 && !p.copies.empty()) { // the copies question next (opened by pump)
        g_release_pending = false;
        g_pending = std::move(p);
        g_ask_next = true;
        return;
    }
    finish_load(g_ask_self, p);
}

void abort_prompts(const char* why)
{
    if (g_name_modal) {
        hide_modal_raw(g_name_modal);
        finish_naming("", why);
    }
    if (g_ask_modal) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, why);
    }
    if (g_ask_next) {
        g_ask_next = false;
        g_pending = {};
        LOG("questions: %s; nothing loaded", why);
    }
}

// Slot 113 of the CommonModal (its button / key handler): only our question is handled here. Without an owner the game
// stores no answer, so the clicked button tells it.
U64 __fastcall ask_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_ask_modal || modal != g_ask_modal || (flag & 0xFF)) return g_ask_click_orig(modal, a, elem, flag);
    bool yes = elem && elem == rp(modal + kOffModalConfirm), no = elem && elem == rp(modal + kOffModalCancel);
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; // Esc may arrive as the cancel button: it loads nothing
    LOG("question: click elem %llX (confirm %d, cancel %d, Esc %d)", elem, yes, no, esc);
    U64 r = call_ask_click_raw(modal, a, elem, flag);
    if (esc) finish_ask(kAskAbort, "Esc; nothing loaded");
    else if (yes || no) finish_ask(yes ? kAskYes : kAskNo, yes ? "yes" : "leave as is");
    return r;
}

// Opens the next question of a load (pieces on other characters, then copies), or loads.
void ask_next(U64 self, AskedLoad p)
{
    if (p.index >= g_outfits.size()) return;
    const Outfit& o = g_outfits[p.index];
    std::string msg, yes;
    if (!p.takes.empty() && !p.takes_answered) {
        p.stage = 1;
        for (const Take& t : p.takes)
            msg += (msg.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.take_item", {{"piece", piece_label(o, o.items[t.k].slot)}, {"who", character_name(t.who)}});
        long long n = static_cast<long long>(p.takes.size());
        msg = lang::n("ask.take", n, {{"piece", piece_label(o, o.items[p.takes[0].k].slot)},
                                      {"who", character_name(p.takes[0].who)}, {"list", msg}});
        yes = lang::n("ask.take_yes", n);
    } else if (!p.copies.empty() && !p.copies_answered) {
        p.stage = 2;
        std::vector<U16> copy_slots;
        for (const auto& [k, uid] : p.copies) copy_slots.push_back(o.items[k].slot);
        long long n = static_cast<long long>(p.copies.size());
        msg = lang::n("ask.copy", n, {{"list", slot_list(o, copy_slots)}});
        yes = lang::n("ask.copy_yes", n);
    } else {
        finish_load(self, p);
        return;
    }
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, o.name.c_str(), msg.c_str(), yes.c_str(), lang::t("ask.leave")) : 0;
    if (!modal) {
        LOG("question %d did not open; those slots stay as they are", p.stage);
        if (p.stage == 1) p.takes_answered = true;
        else p.copies_answered = true;
        ask_next(self, std::move(p));
        return;
    }
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("question %d %llX open (+0xbd %u): %s", g_pending.stage, modal, g_ask_shown, msg.c_str());
}

// pump: the next question of a load, and the asynchronous parts of the last one.

// The next question of a load (opened a moment after the last one closed).
void pump_ask(U64 self)
{
    if (g_ask_next && !g_ask_modal && !g_name_modal && GetTickCount64() - g_modal_ended > 150) {
        g_ask_next = false;
        AskedLoad p = std::move(g_pending);
        g_pending = {};
        if (p.index < g_outfits.size() && g_outfits[p.index].name == p.name) ask_next(self, std::move(p));
    }
}

// While the question is open: notices it closing some other way (Esc, a click outside) or a 3-minute safety stop.
void watch_ask()
{
    if (!g_ask_modal) return;
    unsigned char shown = g_ask_shown;
    rd(g_ask_modal + kOffModalShown, shown);
    if (g_ask_shown && !shown) finish_ask(kAskAbort, "closed without an answer; nothing loaded");
    else if (GetTickCount64() - g_ask_opened > 180000) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, "timed out; nothing loaded");
    }
}

// Optional: without it saving works as before and names stay "Outfit N".
void install_rename_prompt()
{
    if (!have({kRvaTextModalVt, kRvaTextModalClick, kRvaOpenTextModal, kRvaModalParamsInit, kRvaModalParamsFree,
               kRvaFindView, kRvaGetText, kRvaNameInvalid, kRvaHideScript, kOffModalOwner, kOffModalD0})) {
        feature_off("feature.renaming");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaTextModalVt);
    g_find_view = reinterpret_cast<FindViewFn>(g_base + kRvaFindView);
    g_params_init = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsInit);
    g_params_free = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsFree);
    g_get_text = reinterpret_cast<GetTextFn>(g_base + kRvaGetText);
    g_name_invalid = reinterpret_cast<NameInvalidFn>(g_base + kRvaNameInvalid);
    g_hide_script = reinterpret_cast<VoidFn>(g_base + kRvaHideScript);
    g_text_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotTextModalClick]);
    if (!game::patch_vtable(vt, kSlotTextModalClick, reinterpret_cast<void*>(&text_click_hook))) {
        LOG("rename prompt: vtable patch failed; outfits keep their default names");
        feature_off("feature.renaming");
        return;
    }
    if (have({kRvaQueueHide})) // closing the held menu after a prompt (else it stays up until the button is pressed)
        g_queue_hide = reinterpret_cast<QueueHideFn>(g_base + kRvaQueueHide);
    if (have({kRvaBindingInput})) { // ... and releasing its key binding (else the next press only releases it)
        void* t = reinterpret_cast<void*>(g_base + kRvaBindingInput);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&binding_input_hook),
                          reinterpret_cast<void**>(&g_binding_input_orig)) != MH_OK ||
            MH_EnableHook(t) != MH_OK) {
            g_binding_input_orig = nullptr;
            LOG("rename prompt: key binding hook failed; the menu button may need an extra press after a prompt");
        }
    }
    g_open_text_modal = reinterpret_cast<OpenTextModalFn>(g_base + kRvaOpenTextModal); // last: enables the prompt
    LOG("rename prompt ready (TextEdit modal slot %d hooked)", kSlotTextModalClick);
}

// Optional too (needs the rename prompt's helpers): without it a gone piece's slot is simply left as is.
void install_ask_dialog()
{
    if (!g_find_view ||
        !have({kRvaCommonModalVt, kRvaCommonModalClick, kRvaOpenCommonModal, kRvaCommonModalName, kOffModalOwner}) ||
        memcmp(reinterpret_cast<const void*>(g_base + kRvaCommonModalName), "CommonModalMessage", 19) != 0) {
        feature_off("feature.missing_pieces");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaCommonModalVt);
    g_ask_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotCommonModalClick]);
    if (!game::patch_vtable(vt, kSlotCommonModalClick, reinterpret_cast<void*>(&ask_click_hook))) {
        LOG("missing-pieces question: vtable patch failed; gone pieces are left as is");
        feature_off("feature.missing_pieces");
        return;
    }
    g_open_common_modal = reinterpret_cast<OpenCommonModalFn>(g_base + kRvaOpenCommonModal); // last: enables it
    LOG("missing-pieces question ready (CommonModal slot %d hooked)", kSlotCommonModalClick);
}

} // namespace om
