// Keyboard and mouse: the game window's messages, raw input, the mouse wheel hook.
#include "om.h"
#include "input.h"
#include "saves.h"
#include "prompts.h"
#include "wheel_hooks.h"
#include "pads.h"
#include "arc.h"

namespace om {

U64 g_self_lo = 0, g_self_hi = 0;               // (this module: its own input reads see the truth)
std::atomic<bool> g_lmb_held_from_arc{false}; // (#32: the left button held since the arc took it)

// --- Typed characters the prompt drops -------------------------------------------------------------
// The game's text box makes its own characters from the keys: letters (shifted too) and digits, but no Shift / AltGr
// symbols (! + / * ...) or accented letters, though pasting them works. While our prompt is open the characters
// Windows makes for the game window (WM_CHAR) are collected; once typing pauses for two frames, the ones the box didn't
// take are put in at the caret (input +0xd8 / +0xdc: caret and selection end, in characters), which then moves past them
// (+0xf1: the caret-moved flag the game's own keys set, so the caret is drawn there at once).

WNDPROC g_game_wndproc = nullptr;
std::atomic<HWND> g_game_hwnd{nullptr}; // (sleep.cpp posts kWmSleep to it: the window's messages keep coming while the
                                         // HUD is hidden, as in the sleep's fade)
bool sleep_message(); // (sleep.cpp: kWmSleep handled)
bool sleep_fx_timer(WPARAM id); // (sleep.cpp: its effect timer)

// The wheel hook is in (the arc open): it alone counts the notches. The window and raw input routes then only keep
// the wheel from the game (2026-11-06: all three counted, 6 steps per notch, and with 3 outfits the arc went round
// twice and seemed not to move).
std::atomic<bool> g_wheel_hooked{false};

// Is (vk) with the modifiers held now one of the current outfits' hotkeys (in play)?
bool hotkey_held_now(U32 vk)
{
    if (!vk || g_menu_open || g_name_modal || GetTickCount64() - g_peek_hud_at.load() >= 250) return false;
    std::uint8_t mods = static_cast<std::uint8_t>(((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) |
                                                  ((GetKeyState(VK_MENU) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4 : 0));
    bool bare = false; // (no hotkey with exactly these modifiers: the key alone counts, e.g. while Shift sprints)
    for (const auto& h : g_hotkeys) {
        U32 v = h.load();
        if (!v || (v & 0xFF) != vk) continue;
        if (((v >> 8) & 0xFF) == mods) return true;
        bare |= ((v >> 8) & 0xFF) == 0;
    }
    return bare;
}

LRESULT CALLBACK name_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_APP + 0x51 && sleep_message()) return 0; // (kWmSleep: a sleep began or ended)
    if (m == WM_TIMER && sleep_fx_timer(wp)) return 0;
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && hotkey_held_now(U32(wp)))
        return 0; // (an outfit's hotkey: its key does nothing else in play)
    if (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP || m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) {
        U32 vk = m == WM_MBUTTONDOWN || m == WM_MBUTTONUP ? VK_MBUTTON : GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (hotkey_held_now(vk)) return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;
    }
    // The Outfit Arc: while it is open the wheel is its (not the camera's); its key's messages are its own in play.
    if ((m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK) && g_peek_live) return 0; // (#32: the arc's)
    if (m == WM_MOUSEWHEEL) {
        if (g_peek_live) {
            if (!g_wheel_hooked) g_peek_wheel += GET_WHEEL_DELTA_WPARAM(wp); // (else the hook counts it)
            return 0;
        }
    }
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && g_peek_key.load() &&
        wp == g_peek_key.load() && GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal) {
        return 0;
    }
    if (m == WM_CHAR && g_name_modal && wp >= 0x20 && wp != 0x7f && (wp < 0xD800 || wp > 0xDFFF)) {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        if (g_typed.empty()) g_typed_base = g_name_text, g_typed_caret = g_name_caret;
        g_typed += wchar_t(wp);
        g_typed_quiet = 0;
    }
    return CallWindowProcW(g_game_wndproc, w, m, wp, lp);
}

// Once, when our prompt first opens (the game window is in front then); stays in place, idle while no prompt is open.
void watch_game_chars()
{
    if (g_game_wndproc || !game_focused()) return;
    HWND w = GetForegroundWindow();
    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&name_wndproc)));
    g_game_hwnd = w;
    LOG("rename: watching typed characters (window %p, its thread %lu, ui thread %lu)", static_cast<void*>(w),
        GetWindowThreadProcessId(w, nullptr), g_ui_thread);
}

bool game_focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Latches Space / Cross / A presses and left clicks (game window focused, menu open) for the UI thread (pump), and
// follows the game's save files (poll_saves). The keys are only read while the menu is open.
DWORD WINAPI key_thread(LPVOID)
{
    bool space = false, lmb = false, open = false;
    for (U32 tick = 0;; ++tick) {
        Sleep(15);
        if (tick % 16 == 0) poll_saves();
        bool was_open = open;
        open = g_menu_open && !g_name_modal && !g_ask_modal; // not while a prompt is open (re-baselined afterwards)
        if (!open) continue;
        if (!was_open && g_hid_wake) SetEvent(g_hid_wake); // (a PlayStation pad plugged in since: found now)
        U8 pb = pad_buttons();
        bool s = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 || (pb & kPadCross);
        bool m = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!was_open) space = s, lmb = m; // held while the menu opened: not a press
        if (s && !space && game_focused()) g_space_at = GetTickCount64();
        if (m && !lmb && game_focused()) {
            POINT p;
            HWND w = GetForegroundWindow();
            g_click_pos = GetCursorPos(&p) && ScreenToClient(w, &p) ? (U64(U32(p.x)) << 32) | U32(p.y) : ~0ull;
            g_click_at = GetTickCount64();
        }
        space = s;
        lmb = m;
    }
}

// The game reads the keyboard and mouse through raw input (GetRawInputData). The peek's key never reaches it while the
// HUD shows the peek (so a key the game also uses, e.g. Tab's Skill 11, does only the peek), and while the peek is open
// the wheel goes to the peek instead of the camera's distance. Nothing of the game's own code is needed for this.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_raw_input_orig = nullptr;

UINT WINAPI raw_input_hook(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_raw_input_orig(h, cmd, data, size, header);
    if (cmd != RID_INPUT || !data || r == UINT(-1) || r < sizeof(RAWINPUTHEADER)) return r;
    __try {
        RAWINPUT* in = static_cast<RAWINPUT*>(data);
        U32 vk = g_peek_key.load();
        USHORT mouse = peek_mouse_flags(vk);
        bool hud = GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal && !g_ask_modal;
        if (in->header.dwType == RIM_TYPEMOUSE) {
            RAWMOUSE& m = in->data.mouse;
            if (g_peek_live && (m.usButtonFlags & RI_MOUSE_WHEEL)) {
                // (not counted here: the game reads each raw event several times; the hook / window route counts)
                m.usButtonFlags &= ~RI_MOUSE_WHEEL;
                m.usButtonData = 0;
            }
            if (hud && mouse) m.usButtonFlags &= ~mouse;
            if (g_peek_live) m.usButtonFlags &= ~(RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP); // (#32: the arc's)
            for (U32 b : {U32(VK_MBUTTON), U32(VK_XBUTTON1), U32(VK_XBUTTON2)}) // (a hotkey's mouse button)
                if ((m.usButtonFlags & peek_mouse_flags(b)) && hotkey_held_now(b)) m.usButtonFlags &= ~peek_mouse_flags(b);
        } else if (in->header.dwType == RIM_TYPEKEYBOARD) {
            RAWKEYBOARD& k = in->data.keyboard;
            if (hud && vk && !mouse && k.VKey == vk) k.VKey = 0xFF, k.MakeCode = 0, k.Flags &= ~(RI_KEY_E0 | RI_KEY_E1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// While the arc is open, a low-level mouse hook takes the wheel before anything else sees it (2026-11-06: after using a
// pad, the game in its pad mode passed no wheel to its window, so the window / raw input filters saw none and the arc
// didn't move). Installed when the arc opens, removed when it closes (its own thread: such a hook needs a message loop).
std::atomic<DWORD> g_wheel_thread_id{0};
std::atomic<U32> g_wheel_notches{0}; // notches the hook took this opening (on the close's log line)
HANDLE g_wheel_open_event = nullptr;

LRESULT CALLBACK wheel_ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) && g_peek_live && game_focused()) {
        if (wp == WM_LBUTTONDOWN) ++g_peek_clicks; // (#32: a press - a tap or a hold; the game sees no click)
        else ++g_peek_ups;
        return 1;
    }
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && g_peek_live && game_focused()) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        g_peek_wheel += static_cast<SHORT>(HIWORD(m->mouseData));
        ++g_wheel_notches;
        return 1; // (kept from the game: no window message, no raw input)
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI wheel_thread(LPVOID)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // (the thread's queue: the close message can be posted to it)
    g_wheel_thread_id = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(g_wheel_open_event, INFINITE);
        HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, &wheel_ll_proc, g_self, 0);
        if (!h) LOG("Outfit Arc: wheel hook failed (%lu); the window's wheel messages count instead", GetLastError());
        g_wheel_hooked = h != nullptr;
        // (a close's message can be left over from an opening the hook never saw: it only counts with the arc closed,
        // 2026-11-06: a stale one ended the next opening's hook at once and the wheel did nothing for it)
        while (g_peek_live && GetMessageW(&msg, nullptr, 0, 0) > 0)
            if (msg.message == WM_APP && !g_peek_live) break; // (the arc closed)
        if (h) UnhookWindowsHookEx(h);
        g_wheel_hooked = false;
        while (PeekMessageW(&msg, nullptr, WM_APP, WM_APP, PM_REMOVE)) {} // (none left for the next opening)
    }
}

// The arc opened / closed: the wheel hook goes on / off.
void wheel_hook(bool on)
{
    if (on) {
        if (g_wheel_open_event) SetEvent(g_wheel_open_event);
    } else if (DWORD t = g_wheel_thread_id.load()) {
        PostThreadMessageW(t, WM_APP, 0, 0);
    }
}

// #32: while the arc is open - and after, until the button is let go - the game reads the left button as up: it polls
// it through GetKeyboardState and GetAsyncKeyState (its attack; found 2026-11-13), besides raw input and the window's
// messages. Calls from this module (the plugin's own reads) see the truth.
using GkbsFn = BOOL(WINAPI*)(PBYTE);
GkbsFn g_gkbs_orig = nullptr;
BOOL WINAPI gkbs_hook(PBYTE k) // (#32: the game reads the left button here too - its attack, logged 2026-11-13)
{
    const BOOL r = g_gkbs_orig(k);
    if (!r || !k) return r;
    const U64 from = reinterpret_cast<U64>(_ReturnAddress());
    if (from >= g_self_lo && from < g_self_hi) return r;
    if (g_peek_live || g_lmb_held_from_arc) {
        if (k[VK_LBUTTON] & 0x80) {
            g_lmb_held_from_arc = true;
        } else if (!g_peek_live) {
            g_lmb_held_from_arc = false;
        }
        k[VK_LBUTTON] &= 0x7F;
    }
    return r;
}
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetAsyncKeyStateFn g_gaks_orig = nullptr;
SHORT WINAPI gaks_hook(int vk)
{
    const SHORT r = g_gaks_orig(vk);
    if (vk != VK_LBUTTON) return r;
    const U64 from = reinterpret_cast<U64>(_ReturnAddress());
    if (from >= g_self_lo && from < g_self_hi) return r;
    if (g_peek_live) {
        if (r & 0x8000) g_lmb_held_from_arc = true;
        return 0;
    }
    if (g_lmb_held_from_arc) {
        if (r & 0x8000) return 0;
        g_lmb_held_from_arc = false;
    }
    return r;
}

void install_raw_input()
{
    if (g_self) { // (this module's range: its own calls pass)
        const U64 base = reinterpret_cast<U64>(g_self);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_self_lo = base, g_self_hi = base + nt->OptionalHeader.SizeOfImage;
    }
    {
        void* t = nullptr;
        if (MH_CreateHookApiEx(L"user32", "GetKeyboardState", reinterpret_cast<void*>(&gkbs_hook),
                               reinterpret_cast<void**>(&g_gkbs_orig), &t) != MH_OK || MH_EnableHook(t) != MH_OK)
            LOG("Outfit Arc: GetKeyboardState hook FAILED (a left click with the arc open also reaches the game)");
    }
    {
        void* t = nullptr;
        if (MH_CreateHookApiEx(L"user32", "GetAsyncKeyState", reinterpret_cast<void*>(&gaks_hook),
                               reinterpret_cast<void**>(&g_gaks_orig), &t) != MH_OK || MH_EnableHook(t) != MH_OK)
            LOG("Outfit Arc: GetAsyncKeyState hook FAILED (a left click with the arc open also reaches the game)");
    }
    g_wheel_open_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &wheel_thread, nullptr, 0, nullptr)) CloseHandle(t);
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"user32", "GetRawInputData", reinterpret_cast<void*>(&raw_input_hook),
                           reinterpret_cast<void**>(&g_raw_input_orig), &target) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Outfit Arc: raw input hook FAILED (the key and the wheel also reach the game)");
        return;
    }
    LOG("Outfit Arc: raw input hook on (key %s)", peek_key_name(g_peek_key.load()).c_str());
}

} // namespace om
