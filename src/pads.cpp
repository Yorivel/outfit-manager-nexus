// Controllers: PlayStation pads (HID reports) and XInput.
#include "om.h"
#include "pads.h"
#include "arc.h"

namespace om {

// The input in use (the last one touched: a pad, or the mouse / keyboard), set by key_thread; a pad's family picks its
// confirm icon. The game swaps its own prompts the same way.
std::atomic<bool> g_pad_in_use{false}, g_pad_is_ps{false}, g_prompt_changed{false};
thread_local bool t_own_pad = false;      // our own pad reading (XInput, the PlayStation pad's reports): never filtered

// The game's PlayStation pad handles (opened by path: \\?\hid#vid_054c&pid_....), with their report length.
struct PadHandle {
    std::atomic<HANDLE> h{nullptr};
    DWORD len = 0;
    bool dualsense = false;
};
PadHandle g_pad_handles[8];

void note_pad_handle(LPCWSTR path, HANDLE h)
{
    if (!path || h == INVALID_HANDLE_VALUE || t_own_pad) return;
    std::wstring l = path;
    for (wchar_t& c : l) c = static_cast<wchar_t>(std::towlower(c));
    if (l.find(L"vid_054c") == std::wstring::npos) return;
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps{};
    DWORD len = 0;
    if (HidD_GetPreparsedData(h, &pp)) {
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) len = caps.InputReportByteLength;
        HidD_FreePreparsedData(pp);
    }
    if (!len) return;
    bool ds = l.find(L"pid_0ce6") != std::wstring::npos || l.find(L"pid_0df2") != std::wstring::npos;
    for (PadHandle& p : g_pad_handles) {
        HANDLE none = nullptr;
        if (p.h.load() == h) return;
        if (p.h.compare_exchange_strong(none, h)) {
            p.len = len, p.dualsense = ds;
            LOG("Outfit Arc: the game's PlayStation pad handle %p (reports of %lu bytes)", static_cast<void*>(h), len);
            return;
        }
    }
}

const PadHandle* pad_handle(HANDLE h)
{
    if (!h) return nullptr;
    for (const PadHandle& p : g_pad_handles)
        if (p.h.load() == h) return &p;
    return nullptr;
}

U32 crc32_bytes(U32 crc, const unsigned char* d, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// A report with the d-pad's up / down taken out (a diagonal keeps its left / right). Bluetooth reports carry a CRC
// (seeded with 0xA1): written again.
void strip_pad_report(unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;
        else if (r[0] == 0x31 && n >= 11) face = 9;
        else if (r[0] == 0x01 && n >= 7) face = 5;
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;
        else if (r[0] == 0x11 && n >= 9) face = 7;
    }
    if (face < 0 || DWORD(face) >= n) return;
    static const unsigned char kNoUpDown[9] = {8, 2, 2, 2, 8, 6, 6, 6, 8};
    unsigned char d = r[face] & 0x0F;
    if (d > 8 || kNoUpDown[d] == d) return;
    r[face] = static_cast<unsigned char>((r[face] & 0xF0) | kNoUpDown[d]);
    if ((r[0] == 0x31 || r[0] == 0x11) && n >= 78) {
        unsigned char seed = 0xA1;
        U32 c = crc32_bytes(crc32_bytes(0, &seed, 1) , r, 74);
        // (crc32_bytes chains: the seed byte, then the report's first 74 bytes)
        std::memcpy(r + 74, &c, 4);
    }
}

// Overlapped reads of a pad handle: the buffer is filled later; filtered when the game asks for the result.
struct PendingRead {
    std::atomic<OVERLAPPED*> ov{nullptr};
    unsigned char* buf = nullptr;
    DWORD len = 0;
    bool dualsense = false;
};
PendingRead g_pending_reads[16];

using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResultFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultExFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
ReadFileFn g_read_file_orig = nullptr;
GetOverlappedResultFn g_ov_result_orig = nullptr;
GetOverlappedResultExFn g_ov_result_ex_orig = nullptr;

BOOL WINAPI read_file_hook(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL r = g_read_file_orig(h, buf, n, got, ov);
    if (t_own_pad || !buf) return r;
    const PadHandle* p = pad_handle(h);
    if (!p || n < p->len) return r;
    DWORD err = GetLastError();
    if (r && peek_pad_owned()) {
        DWORD k = got ? *got : n;
        __try {
            strip_pad_report(static_cast<unsigned char*>(buf), k ? k : n, p->dualsense);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    } else if (!r && err == ERROR_IO_PENDING && ov) {
        for (PendingRead& q : g_pending_reads) {
            OVERLAPPED* none = nullptr;
            if (q.ov.load() == ov || q.ov.compare_exchange_strong(none, ov)) {
                q.buf = static_cast<unsigned char*>(buf), q.len = n, q.dualsense = p->dualsense;
                q.ov = ov;
                break;
            }
        }
    }
    SetLastError(err);
    return r;
}

void finish_pending_read(LPOVERLAPPED ov, BOOL ok, LPDWORD got)
{
    if (!ov) return;
    for (PendingRead& q : g_pending_reads) {
        if (q.ov.load() != ov) continue;
        if (ok && peek_pad_owned()) {
            __try {
                strip_pad_report(q.buf, got && *got ? *got : q.len, q.dualsense);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (ok) q.ov = nullptr; // (the slot is free; the game's next read takes it again)
        return;
    }
}

BOOL WINAPI ov_result_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait)
{
    BOOL r = g_ov_result_orig(h, ov, got, wait);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

BOOL WINAPI ov_result_ex_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, DWORD ms, BOOL alertable)
{
    BOOL r = g_ov_result_ex_orig(h, ov, got, ms, alertable);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

// Xbox pads: the game's xinput1_4 (by name or by ordinal 100, the one with the guide button) gets the d-pad's up / down
// taken out of its button word.
using XGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XGetStateFn g_xget_orig = nullptr, g_xget100_orig = nullptr;

DWORD xget_filter(DWORD r, void* state)
{
    if (r == ERROR_SUCCESS && state && !t_own_pad && peek_pad_owned()) {
        WORD& b = *reinterpret_cast<WORD*>(static_cast<unsigned char*>(state) + 4); // (XINPUT_STATE: packet, then buttons)
        if (b & 0x0003) b &= static_cast<WORD>(~0x0003);
    }
    return r;
}
DWORD WINAPI xget_hook(DWORD i, void* s) { return xget_filter(g_xget_orig(i, s), s); }
DWORD WINAPI xget100_hook(DWORD i, void* s) { return xget_filter(g_xget100_orig(i, s), s); }

void install_pad_filters()
{
    struct { const wchar_t* dll; const char* name; void* fn; void** orig; } hooks[] = {
        {L"kernelbase", "ReadFile", reinterpret_cast<void*>(&read_file_hook), reinterpret_cast<void**>(&g_read_file_orig)},
        {L"kernelbase", "GetOverlappedResult", reinterpret_cast<void*>(&ov_result_hook), reinterpret_cast<void**>(&g_ov_result_orig)},
        {L"kernelbase", "GetOverlappedResultEx", reinterpret_cast<void*>(&ov_result_ex_hook), reinterpret_cast<void**>(&g_ov_result_ex_orig)},
    };
    int ok = 0;
    for (auto& h : hooks) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(h.dll, h.name, h.fn, h.orig, &target) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
        else LOG("Outfit Arc: %s filter failed", h.name);
    }
    if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) {
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget_hook), reinterpret_cast<void**>(&g_xget_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, MAKEINTRESOURCEA(100))))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget100_hook), reinterpret_cast<void**>(&g_xget100_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
    }
    LOG("Outfit Arc: pad filters on (%d of 5)", ok);
}
constexpr U8 kPadTouched = 2; // any button, trigger or stick in use (the pad is the input in use),

struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
constexpr WORD kXInputA = 0x1000; // XINPUT_GAMEPAD_A
constexpr WORD kXInputUp = 0x0001, kXInputDown = 0x0002, kXInputLB = 0x0100; // XINPUT_GAMEPAD_DPAD_UP / _DOWN / LEFT_SHOULDER

U8 xinput_buttons()
{
    static XInputGetStateFn get = [] {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll))
                if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
                    LOG("controller input (Xbox pads): %ls", dll);
                    return f;
                }
        LOG("controller input: no XInput found (Xbox pads won't work in the Outfits tab)");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    static U64 skip_until[4] = {};
    if (!get) return 0;
    U64 now = GetTickCount64();
    U8 b = 0;
    struct Own { bool was = t_own_pad; Own() { t_own_pad = true; } ~Own() { t_own_pad = was; } } own; // (unfiltered)
    for (DWORD i = 0; i < 4; ++i) {
        if (now < skip_until[i]) continue;
        XState s{};
        if (get(i, &s) != ERROR_SUCCESS) {
            skip_until[i] = now + 2000;
            continue;
        }
        if (s.Gamepad.wButtons & kXInputA) b |= kPadCross;
        if (s.Gamepad.wButtons & kXInputLB) b |= kPadL1;
        if (s.Gamepad.wButtons & kXInputUp) b |= kPadUp;
        if (s.Gamepad.wButtons & kXInputDown) b |= kPadDown;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 64 || s.Gamepad.bRightTrigger > 64 ||
            std::abs(s.Gamepad.sThumbLX) > 12000 || std::abs(s.Gamepad.sThumbLY) > 12000 ||
            std::abs(s.Gamepad.sThumbRX) > 12000 || std::abs(s.Gamepad.sThumbRY) > 12000)
            b |= kPadTouched;
    }
    return b;
}

// PlayStation pads over HID. The face-button byte (Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80) by report, as
// documented by the community (SDL, DS4Windows): DualSense USB 0x01 [8], Bluetooth 0x31 (full mode, as Steam sets it)
// [9], Bluetooth simple 0x01 [5]; DualShock 4 USB 0x01 [5], Bluetooth 0x11 [7].
std::atomic<U8> g_hid_buttons{0};
HANDLE g_hid_wake = nullptr; // set when the menu opens: a thread without a pad looks again

struct PadKind {
    USHORT pid;
    bool dualsense;
};
constexpr USHORT kSonyVid = 0x054C;
constexpr PadKind kPads[] = {{0x0CE6, true}, {0x0DF2, true}, {0x05C4, false}, {0x09CC, false}, {0x0BA0, false}};

// The sticks are the four bytes right after the report id (DualShock 4 over Bluetooth: two later), 0x80 = centred; the
// face byte's low nibble is the d-pad (8 = none), the next two bytes hold the shoulder / stick / menu buttons.
U8 hid_parse(const unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1, sticks = 1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;                   // USB
        else if (r[0] == 0x31 && n >= 11) face = 9, sticks = 2;  // Bluetooth, full
        else if (r[0] == 0x01 && n >= 7) face = 5;               // Bluetooth, simple
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;                    // USB (and Bluetooth simple)
        else if (r[0] == 0x11 && n >= 9) face = 7, sticks = 3;   // Bluetooth
    }
    if (face < 0 || DWORD(face + 2) >= n) return 0;
    U8 b = (r[face] & 0x20) ? kPadCross : 0;
    U8 dpad = r[face] & 0x0F; // (0 up, clockwise to 7 up-left; 8 none)
    if (dpad == 0 || dpad == 1 || dpad == 7) b |= kPadUp;
    if (dpad >= 3 && dpad <= 5) b |= kPadDown;
    if (r[face + 1] & 0x01) b |= kPadL1; // (L1: the first bit of the next byte, DualSense and DualShock 4 alike)
    bool touched = (r[face] & 0xF0) || (r[face] & 0x0F) != 8 || r[face + 1] || (r[face + 2] & 0x03);
    for (int s = 0; s < 4 && !touched; ++s) touched = std::abs(int(r[sticks + s]) - 0x80) > 48;
    return b | (touched ? kPadTouched : 0);
}

// The first connected PlayStation pad: an open handle (shared with the game and Steam) and its input report size.
HANDLE hid_open(bool& dualsense, DWORD& report_len)
{
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA itf{sizeof itf};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &itf); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        std::vector<char> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!need || !SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ours = false;
        if (HidD_GetAttributes(h, &at) && at.VendorID == kSonyVid)
            for (const PadKind& k : kPads)
                if (at.ProductID == k.pid) ours = true, dualsense = k.dualsense;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (ours && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength) {
                report_len = caps.InputReportByteLength;
                found = h;
                LOG("controller input (PlayStation pad): %s, product %04X, reports of %lu bytes",
                    dualsense ? "DualSense" : "DualShock 4", at.ProductID, report_len);
            }
            HidD_FreePreparsedData(pp);
        }
        if (found != h) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Waits for a PlayStation pad's reports and keeps its buttons in g_hid_buttons. Without a pad (or once it is gone),
// it waits for the menu to open and looks again.
DWORD WINAPI hid_thread(LPVOID)
{
    t_own_pad = true; // (our reading: never filtered, never taken for the game's)
    for (;;) {
        bool dualsense = false;
        DWORD len = 0;
        HANDLE h = hid_open(dualsense, len);
        if (h == INVALID_HANDLE_VALUE) {
            g_hid_buttons = 0;
            WaitForSingleObject(g_hid_wake, INFINITE);
            continue;
        }
        std::vector<unsigned char> r(len);
        DWORD got = 0;
        while (ReadFile(h, r.data(), len, &got, nullptr)) g_hid_buttons = hid_parse(r.data(), got, dualsense);
        LOG("controller input: the PlayStation pad stopped answering (disconnected?)");
        CloseHandle(h);
        g_hid_buttons = 0;
    }
}

// The pads' buttons, and which input is in use: a touched pad switches to it (its family too); the mouse moving or
// clicking, or Space, switches back. A change asks pump to re-show the slots (g_prompt_changed).
U8 pad_buttons_raw() { return xinput_buttons() | g_hid_buttons.load(); }

U8 pad_buttons()
{
    U8 x = xinput_buttons(), h = g_hid_buttons.load();
    static POINT last{};
    POINT p{};
    bool mouse = GetCursorPos(&p) && (std::abs(p.x - last.x) > 2 || std::abs(p.y - last.y) > 2);
    last = p;
    // A PlayStation pad through Steam Input shows up on both readers; the game then sees the virtual Xbox pad (and shows
    // Xbox prompts), so XInput wins the icon family.
    bool pad = (x | h) & kPadTouched, ps = g_pad_is_ps;
    if (h & kPadTouched) ps = true;
    if (x & kPadTouched) ps = false;
    bool keys = (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    bool in_use = pad ? true : (mouse || keys) ? false : g_pad_in_use.load();
    if (in_use != g_pad_in_use || (in_use && ps != g_pad_is_ps)) {
        g_pad_in_use = in_use, g_pad_is_ps = ps;
        g_prompt_changed = true;
    }
    return x | h;
}

} // namespace om
