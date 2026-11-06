// Controllers: PlayStation pads (HID reports) and XInput.
#pragma once
#include "om.h"

namespace om {

extern std::atomic<bool> g_pad_in_use, g_pad_is_ps, g_prompt_changed;
void note_pad_handle(LPCWSTR path, HANDLE h);
void install_pad_filters();
extern HANDLE g_hid_wake;
DWORD WINAPI hid_thread(LPVOID);
U8 pad_buttons_raw();
U8 pad_buttons();

} // namespace om
