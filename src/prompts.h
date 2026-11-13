// The game's dialogs: the rename prompt, typed characters, yes / no questions.
#pragma once
#include "om.h"

namespace om {

extern int kSlotTextModalClick;
extern int kSlotCommonModalClick;
extern std::atomic<U64> g_name_modal;
extern U64 g_modal_ended;
extern QueueHideFn g_queue_hide;
extern bool g_release_pending;
void note_menu_binding();
void release_menu_button(U64 self);
extern std::mutex g_typed_mu;
extern std::wstring g_typed;
extern std::string g_typed_base;
extern U32 g_typed_caret;
extern int g_typed_quiet;
extern std::string g_name_text;
extern U32 g_name_caret;
void open_rename_prompt(U64 self, size_t index);
void watch_rename_prompt();
extern std::atomic<U64> g_ask_modal;
extern U64 g_ask_self;
bool open_barber_question(U64 root, const char* title, const char* msg, const char* yes, const char* no);
extern U64 g_ask_opened;
extern unsigned char g_ask_shown;
extern AskedLoad g_pending;
U64 open_common_modal_raw(U64 self, const char* title, const char* message, const char* yes, const char* no);
extern bool g_ask_next;
void abort_prompts(const char* why);
void ask_next(U64 self, AskedLoad p);
void pump_ask(U64 self);
void watch_ask();
void install_rename_prompt();
void install_ask_dialog();

} // namespace om
