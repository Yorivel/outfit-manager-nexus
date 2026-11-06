// The gear wheel's compiled UI files, patched as the game loads them.
#include "om.h"
#include "ui_hook.h"
#include "wheel_hooks.h"

namespace om {

UiParseFn g_ui_parse_orig = nullptr;
U64 g_ui_parse_ret = 0;                                // the parser call's return address (the loader's call)
std::int64_t g_ui_path_inline = 0, g_ui_path_heap = 0; // the path buffer / its heap pointer, relative to the stream
std::string g_ui_hook_note;                            // how the hook went in (logged by the init thread)
std::mutex g_ui_mutex;
std::deque<std::vector<U8>> g_ui_patched; // the patched files, alive for the run

bool ui_copy(U64 from, void* to, size_t n)
{
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ui_stream_path(U64 stream)
{
    char buf[0x106] = {};
    U64 heap = 0;
    U64 src = rd(stream + g_ui_path_heap, heap) && plausible(heap) ? heap : stream + g_ui_path_inline;
    if (!ui_copy(src, buf, sizeof buf - 1)) return {};
    buf[sizeof buf - 1] = 0;
    for (char* c = buf; *c; ++c)
        if (*c < 0x20 || *c > 0x7e) return {};
    std::string r = buf;
    return r.size() > 9 && _stricmp(r.c_str() + r.size() - 9, ".uibinary") == 0 ? r : std::string();
}

void ui_patch_stream(U64 stream, const std::string& path, unsigned what)
{
    U64 data = 0;
    U32 size = 0, pos = 0;
    if (!rd(stream + 0x20, data) || !rd(stream + 0x28, size) || !rd(stream + 0x2c, pos) || !plausible(data) || !size ||
        size > (64u << 20) || pos) {
        LOG("gear wheel UI: %s left as it is (its stream isn't as expected: %llX, %u bytes, at %u)", path.c_str(), data,
            size, pos);
        return;
    }
    std::vector<U8> in(size), out;
    if (!ui_copy(data, in.data(), size)) {
        LOG("gear wheel UI: %s left as it is (bytes unreadable)", path.c_str());
        return;
    }
    uibin::Report r;
    if (!uibin::patch(in, what, out, r)) {
        LOG("gear wheel UI: %s left as it is: %s", path.c_str(), r.error.c_str());
        return;
    }
    std::lock_guard lock(g_ui_mutex);
    g_ui_patched.push_back(std::move(out));
    const std::vector<U8>& kept = g_ui_patched.back();
    *reinterpret_cast<U64*>(stream + 0x20) = reinterpret_cast<U64>(kept.data());
    *reinterpret_cast<U32*>(stream + 0x28) = U32(kept.size());
    if (r.headers) ++g_ui_tab_added;
    LOG("gear wheel UI: %s: %d tab header(s), %d value text(s), %d peek(s) added (%u -> %zu bytes)", path.c_str(),
        r.headers, r.values, r.peeks, size, kept.size());
}

U64 __fastcall ui_parse_hook(U64 loader, U64 stream)
{
    if (reinterpret_cast<U64>(_ReturnAddress()) == g_ui_parse_ret) {
        try {
            std::string path = ui_stream_path(stream);
            if (unsigned what = path.empty() ? 0 : uibin::wants(path)) ui_patch_stream(stream, path, what);
        } catch (...) { // (out of memory: the file stays as it is)
        }
    }
    return g_ui_parse_orig(loader, stream);
}

// At load (DllMain), before the game builds its UI; again from the init thread if that didn't work.
bool install_ui_patch()
{
    if (g_ui_parse_orig) return true;
    if (!g_base) g_base = game::main_module().base;
    // mov rax,[rsp+70]; mov [rbp+..],rax; mov eax,[rsp+7C]; mov [rbp+..],eax; mov [rbp+..],r12d; lea rdx,[rbp+s];
    // mov rcx,rsi; call parser  (unique in the 10-30, 11-01b and 11-04 exes)
    U64 site = game::find_unique("48 8B 44 24 70 48 89 45 ?? 8B 44 24 7C 89 45 ?? 44 89 65 ?? 48 8D 55 ?? 48 8B CE E8");
    if (!site) return g_ui_hook_note = "the UI loader's parser call not found", false;
    std::int8_t stream_at = 0;
    std::int32_t rel = 0;
    rd(site + 23, stream_at), rd(site + 28, rel);
    // lea r15,[rbp+i]; mov rcx,[rbp+h]; test rcx,rcx; cmovne r15,rcx: the path buffer, inline or on the heap
    const U8 pat[] = {0x4C, 0x8D, 0x7D, 0, 0x48, 0x8B, 0x8D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x4C, 0x0F, 0x45, 0xF9};
    U64 hit = 0;
    int hits = 0;
    for (U64 a = site - 0x600; a < site; ++a) {
        bool ok = true;
        for (size_t k = 0; k < sizeof pat && ok; ++k)
            ok = k == 3 || (k >= 7 && k <= 10) || *reinterpret_cast<const U8*>(a + k) == pat[k];
        if (ok) hit = a, ++hits;
    }
    if (hits != 1) return g_ui_hook_note = "the UI loader's path buffer not found (" + std::to_string(hits) + ")", false;
    std::int8_t inl = 0;
    std::int32_t heap = 0;
    rd(hit + 3, inl), rd(hit + 7, heap);
    g_ui_path_inline = std::int64_t(inl) - stream_at;
    g_ui_path_heap = std::int64_t(heap) - stream_at;
    U64 call = site + 27, parser = call + 5 + rel;
    g_ui_parse_ret = call + 5;
    MH_STATUS init = MH_Initialize();
    void* fn = reinterpret_cast<void*>(parser);
    if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(fn, reinterpret_cast<void*>(&ui_parse_hook), reinterpret_cast<void**>(&g_ui_parse_orig)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        g_ui_parse_orig = nullptr;
        return g_ui_hook_note = "the UI parser hook failed", false;
    }
    char t[160];
    std::snprintf(t, sizeof t, "parser +%llX (call +%llX), path at stream%+lld / [stream%+lld]", parser - g_base,
                  call - g_base, g_ui_path_inline, g_ui_path_heap);
    g_ui_hook_note = t;
    return true;
}

} // namespace om
