#pragma once
// Outfit Manager's words in the player's language: "key = text" files in <plugin folder>\Outfit Manager\lang\
// (<code>.txt shipped with the mod, <code>.custom.txt the player's own lines, which win), English built in
// (lang_en.inc, generated from the mod's lang\en.txt by tools/lang_gen.py) for anything missing.
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace lang {

struct Language {
    const char* code;    // file name: <code>.txt
    const char* name;    // in its own language (the Language setting's value)
    const char* aliases; // how the game's options file and Steam may name it (lower case, letters and digits only)
};
extern const Language kLanguages[];
extern const size_t kLanguageCount;

// Loads `code` (a kLanguages code; anything else: English) from dir (ending in a separator).
void load(const std::wstring& dir, const std::string& code);
const std::string& code();

// The game's text language: its options file (_textLanguage), else Steam's language for the game, else English.
// how: where it came from (for the log).
std::string detect(const std::wstring& plugin_dir, std::string& how);

// A line by key (stable until the next load); the key itself if no table has it.
const char* t(const char* key);

using Args = std::initializer_list<std::pair<const char*, std::string>>;
// A line with its {name} parts filled in ({sp} is a space).
std::string f(const char* key, Args args = {});
// A counted line: key.one / .few / ... by the number's plural form in this language (missing: .other), {n} = count.
std::string n(const char* key, long long count, Args args = {});

}
