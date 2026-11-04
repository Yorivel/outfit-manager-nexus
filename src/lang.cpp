#include "lang.h"

#include <windows.h>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace lang {

const Language kLanguages[] = {
    {"en", "English", "english eng en englishus enus"},
    {"fr", "Français", "french fre fra fr"},
    {"de", "Deutsch", "german ger deu de"},
    {"it", "Italiano", "italian ita it"},
    {"es", "Español (España)", "spanish spa spaes es spanishes spain"},
    {"es-mx", "Español (Latinoamérica)", "latam spamx esmx spanishmx spanishlatam latinamericanspanish mexicanspanish"},
    {"pt-br", "Português (Brasil)", "brazilian porbr ptbr portuguesebr portuguese brazilianportuguese"},
    {"pl", "Polski", "polish pol pl"},
    {"ru", "Русский", "russian rus ru"},
    {"tr", "Türkçe", "turkish tur tr"},
    {"ar", "العربية", "arabic ara ar"},
    {"ja", "日本語", "japanese jpn ja jp"},
    {"ko", "한국어", "koreana korean kor ko"},
    {"zh-cn", "简体中文", "schinese zhocn chineseprc simplifiedchinese chinesesimplified chs zhcn"},
    {"zh-tw", "繁體中文", "tchinese zhotw traditionalchinese chinesetraditional cht zhtw"},
};
const size_t kLanguageCount = sizeof kLanguages / sizeof kLanguages[0];

namespace {

using Table = std::unordered_map<std::string, std::string>;

const Table& english()
{
    static const Table t = [] {
        static const char* const kEn[][2] = {
#include "lang_en.inc"
        };
        Table m;
        for (const auto& e : kEn) m[e[0]] = e[1];
        return m;
    }();
    return t;
}

Table g_table;          // the chosen language's lines (shipped, then the player's)
std::string g_code = "en";

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "key = text" lines (UTF-8, a BOM allowed; # comments). Returns the number of lines read, -1 if there is no file.
int read_file(const std::wstring& path, Table& into)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return -1;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    int n = 0;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq)), val = trim(l.substr(eq + 1));
        if (key.empty()) continue;
        into[key] = val;
        ++n;
    }
    return n;
}

std::string norm(const std::string& s)
{
    std::string o;
    for (unsigned char c : s)
        if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
    return o;
}

// A language by any of its names; "" if none.
std::string by_alias(const std::string& name)
{
    std::string n = norm(name);
    if (n.empty()) return "";
    for (size_t i = 0; i < kLanguageCount; ++i) {
        std::istringstream a(kLanguages[i].aliases);
        for (std::string w; a >> w;)
            if (w == n) return kLanguages[i].code;
    }
    return "";
}

std::string read_all(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The value of attribute `attr` in the element that has Name="<name>".
std::string xml_select(const std::string& xml, const char* name, const char* attr)
{
    size_t p = xml.find(std::string("Name=\"") + name + "\"");
    if (p == std::string::npos) return "";
    size_t end = xml.find('>', p);
    size_t a = xml.find(std::string(attr) + "=\"", p);
    if (a == std::string::npos || a > end) return "";
    a += std::strlen(attr) + 2;
    return xml.substr(a, xml.find('"', a) - a);
}

// The first "language" value in an appmanifest (UserConfig / MountedConfig).
std::string acf_language(const std::string& acf)
{
    size_t p = acf.find("\"language\"");
    if (p == std::string::npos) return "";
    size_t a = acf.find('"', p + 10);
    if (a == std::string::npos) return "";
    size_t b = acf.find('"', a + 1);
    return b == std::string::npos ? "" : acf.substr(a + 1, b - a - 1);
}

int plural_form(long long n, const std::string& code, const char*& form)
{
    long long m10 = n % 10, m100 = n % 100;
    if (code == "ja" || code == "ko" || code == "zh-cn" || code == "zh-tw") return form = "other", 0;
    if (code == "pl") {
        form = n == 1 ? "one" : m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14) ? "few" : "many";
        return 0;
    }
    if (code == "ru") {
        form = m10 == 1 && m100 != 11 ? "one" : m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14) ? "few" : "many";
        return 0;
    }
    if (code == "ar") {
        form = n == 0 ? "zero" : n == 1 ? "one" : n == 2 ? "two" : m100 >= 3 && m100 <= 10 ? "few" : m100 >= 11 ? "many" : "other";
        return 0;
    }
    if (code == "fr" || code == "pt-br") return form = n <= 1 ? "one" : "other", 0;
    return form = n == 1 ? "one" : "other", 0;
}

} // namespace

void load(const std::wstring& dir, const std::string& code)
{
    g_table.clear();
    g_code = "en";
    for (size_t i = 0; i < kLanguageCount; ++i)
        if (code == kLanguages[i].code) g_code = code;
    std::wstring base = dir + std::wstring(g_code.begin(), g_code.end());
    int shipped = g_code == "en" ? 0 : read_file(base + L".txt", g_table);
    int own = read_file(base + L".custom.txt", g_table);
    LOG("language: %s (%d lines shipped%s, %d of the player's)", g_code.c_str(), shipped < 0 ? 0 : shipped,
        shipped < 0 ? ": no file" : "", own < 0 ? 0 : own);
}

const std::string& code() { return g_code; }

std::string detect(const std::wstring& plugin_dir, std::string& how)
{
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        std::string xml = read_all(std::wstring(local) + L"\\Pearl Abyss\\CD\\save\\user_engine_option_save.xml");
        std::string v = xml_select(xml, "_textLanguage", "_select");
        if (!v.empty()) {
            std::string c = by_alias(v);
            how = "the game's options (\"" + v + "\")" + (c.empty() ? ", not known" : "");
            if (!c.empty()) return c;
        }
    }
    // <library>\steamapps\common\<game>\bin64\ -> <library>\steamapps\appmanifest_*.acf naming the game's folder
    std::wstring d = plugin_dir;
    auto up = [](std::wstring& p) {
        if (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        size_t s = p.find_last_of(L"\\/");
        std::wstring last = s == std::wstring::npos ? p : p.substr(s + 1);
        p = s == std::wstring::npos ? L"" : p.substr(0, s + 1);
        return last;
    };
    up(d);                        // bin64
    std::wstring game = up(d);    // the game's folder
    up(d);                        // common
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((d + L"appmanifest_*.acf").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::string want = "\"installdir\"";
        std::string game8; // (an ASCII folder name)
        for (wchar_t c : game) game8 += static_cast<char>(c);
        do {
            std::string acf = read_all(d + fd.cFileName);
            size_t p = acf.find(want);
            if (p == std::string::npos || acf.find("\"" + game8 + "\"", p) != acf.find('"', p + want.size())) continue;
            std::string v = acf_language(acf), c = by_alias(v);
            how = "Steam (\"" + v + "\")" + (c.empty() ? ", not known" : "");
            if (!c.empty()) {
                FindClose(h);
                return c;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (how.empty()) how = "nothing found";
    return "en";
}

const char* t(const char* key)
{
    auto i = g_table.find(key);
    if (i != g_table.end() && !i->second.empty()) return i->second.c_str();
    auto e = english().find(key);
    return e != english().end() ? e->second.c_str() : key;
}

namespace {
// {name} parts filled from args; {sp} is a space; unknown names stay as written.
std::string fill(std::string s, const std::vector<std::pair<const char*, std::string>>& args)
{
    for (size_t p = s.find('{'); p != std::string::npos; p = s.find('{', p)) {
        size_t q = s.find('}', p);
        if (q == std::string::npos) break;
        std::string name = s.substr(p + 1, q - p - 1), val;
        bool found = name == "sp";
        if (found) val = " ";
        for (const auto& [k, v] : args)
            if (name == k) val = v, found = true;
        if (!found) {
            p = q + 1;
            continue;
        }
        s.replace(p, q - p + 1, val);
        p += val.size();
    }
    return s;
}
}

std::string f(const char* key, Args args) { return fill(t(key), std::vector<std::pair<const char*, std::string>>(args)); }

std::string n(const char* key, long long count, Args args)
{
    const char* form = "other";
    plural_form(count, g_code, form);
    std::string kf = std::string(key) + "." + form, ko = std::string(key) + ".other";
    // (the language's own form, its .other, then English's)
    const std::string& k = g_table.count(kf) ? kf : g_table.count(ko) ? ko : english().count(kf) ? kf : ko;
    std::vector<std::pair<const char*, std::string>> all(args);
    std::string num = std::to_string(count);
    all.push_back({"n", num});
    return fill(t(k.c_str()), all);
}

}
