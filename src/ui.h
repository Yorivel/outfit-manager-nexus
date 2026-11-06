// UI element helpers: classes, layout checks, labels, images, dumps, messages.
#pragma once
#include "om.h"

namespace om {

bool is_element(U64 e);
std::string describe(U64 p);
void dump_quickslotinfo();
void dump_menu(U64 self, const char* why);
bool has_vt(U64 obj, U64 vt_rva);
extern std::string g_layout_bad;
extern bool g_layout_toast_done;
bool layout_ok(U64 self);
U64 __fastcall build_hook(U64 self, U64 out, U64 a, U64 b);
U64 find_by_class(U64 elem, const char* cls, int max_depth = 8, int depth = 0);
U64 find_parent_of_class(U64 elem, const char* cls, int max_depth = 8, int depth = 0);
void show_header(U64 h);
U64 outfits_header(U64 self);
void set_label(U64 h);
bool set_class(U64 elem, const char* cls, bool on);
bool swap_class(U64 elem, std::string& applied, const std::string& want);
bool has_class(U64 elem, const char* cls);
int set_image_raw(U64 self, U64 elem, const char* name);
extern SlotImage g_slot_image[kOutfitSlots];
extern bool g_rehide_logged;
void notify(U64 self, const std::string& text);
void report(U64 self, const std::string& text);

} // namespace om
