// Save key: which save a save.save is, by its contents rather than its bytes. A save editor changes the bytes (and so
// the hash the mod binds outfits with), but not when and where the save was made: the save's own transform record
// (TransformFieldSaveData: _fieldTimeRaw, the world clock, and _position). Every save of every playthrough has its
// own clock, growing within a playthrough (2026-11-12: seven saves, three playthroughs, all different; two saves made
// at the same spot told apart by the clock). Read only, from a copy in memory.
//
// The file (as documented by the community save editor; this is an independent implementation): 0x80-byte header -
// "SAVE", u16 version at +4, u32 unpacked size at +0x12, u32 payload size at +0x16, 16-byte ChaCha20 nonce at +0x1A
// (u32 block counter + 12-byte nonce); payload at +0x80 = ChaCha20 of an LZ4 block. The unpacked data starts with
// its schema (types and their fields by name), so the record is found by name, not by a fixed place.
#include "om.h"
#include "saves.h"
#include <cstring>
#include <string>
#include <vector>

namespace om {

namespace {

// The key per save version (derived from public constants; version 2 is the current one).
const unsigned char kKeyV1[32] = {0x9A, 0x4A, 0xE9, 0x11, 0x7F, 0x9F, 0x76, 0x88, 0x14, 0x8C, 0x64, 0x93, 0xC2, 0x5D, 0xCB, 0x34,
                                  0x9A, 0x62, 0x0C, 0xE3, 0x5F, 0x0D, 0xDA, 0x00, 0x1D, 0xEA, 0x43, 0x81, 0x1D, 0x47, 0xAA, 0x00};
const unsigned char kKeyV2[32] = {0x9A, 0x4B, 0xEB, 0x12, 0x7F, 0x9E, 0x74, 0x8B, 0x14, 0x8D, 0x66, 0x90, 0xC2, 0x5C, 0xC9, 0x37,
                                  0x9A, 0x31, 0x5B, 0xD5, 0x6C, 0x28, 0xAF, 0x63, 0x19, 0xFD, 0x55, 0x9F, 0x11, 0x52, 0xAC, 0x00};

U32 le32(const unsigned char* p) { return U32(p[0]) | U32(p[1]) << 8 | U32(p[2]) << 16 | U32(p[3]) << 24; }
U32 rotl(U32 v, int n) { return (v << n) | (v >> (32 - n)); }

void chacha20(const unsigned char key[32], const unsigned char nonce16[16], unsigned char* data, size_t n)
{
    U32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) s[4 + i] = le32(key + i * 4);
    for (int i = 0; i < 4; ++i) s[12 + i] = le32(nonce16 + i * 4); // (word 12: the block counter)
    for (size_t pos = 0; pos < n; pos += 64) {
        U32 w[16];
        std::memcpy(w, s, sizeof w);
        auto qr = [&](int a, int b, int c, int d) {
            w[a] += w[b], w[d] = rotl(w[d] ^ w[a], 16);
            w[c] += w[d], w[b] = rotl(w[b] ^ w[c], 12);
            w[a] += w[b], w[d] = rotl(w[d] ^ w[a], 8);
            w[c] += w[d], w[b] = rotl(w[b] ^ w[c], 7);
        };
        for (int r = 0; r < 10; ++r) {
            qr(0, 4, 8, 12), qr(1, 5, 9, 13), qr(2, 6, 10, 14), qr(3, 7, 11, 15);
            qr(0, 5, 10, 15), qr(1, 6, 11, 12), qr(2, 7, 8, 13), qr(3, 4, 9, 14);
        }
        unsigned char block[64];
        for (int i = 0; i < 16; ++i) {
            const U32 v = w[i] + s[i];
            block[i * 4] = static_cast<unsigned char>(v), block[i * 4 + 1] = static_cast<unsigned char>(v >> 8);
            block[i * 4 + 2] = static_cast<unsigned char>(v >> 16), block[i * 4 + 3] = static_cast<unsigned char>(v >> 24);
        }
        for (size_t i = 0; i < 64 && pos + i < n; ++i) data[pos + i] ^= block[i];
        ++s[12];
    }
}

// LZ4 block format: tokens (literal length, match length), literals, a 2-byte back offset. False if malformed.
bool lz4_block(const unsigned char* src, size_t n, std::vector<unsigned char>& out, size_t expect)
{
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    while (i < n) {
        const unsigned token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15)
            for (unsigned char b = 255; b == 255 && i < n;) lit += (b = src[i++]);
        if (i + lit > n || out.size() + lit > expect) return false;
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i >= n) break; // (the last sequence has literals only)
        if (i + 2 > n) return false;
        const size_t off = src[i] | (src[i + 1] << 8);
        i += 2;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15)
            for (unsigned char b = 255; b == 255 && i < n;) len += (b = src[i++]);
        if (!off || off > out.size() || out.size() + len > expect) return false;
        const size_t from = out.size() - off;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]); // (byte by byte: the copy may overlap)
    }
    return out.size() == expect;
}

struct Reader {
    const std::vector<unsigned char>& d;
    size_t p;
    bool ok = true;
    U32 u32()
    {
        if (p + 4 > d.size()) return ok = false, 0;
        const U32 v = le32(d.data() + p);
        p += 4;
        return v;
    }
    U16 u16()
    {
        if (p + 2 > d.size()) return ok = false, 0;
        const U16 v = static_cast<U16>(d[p] | d[p + 1] << 8);
        p += 2;
        return v;
    }
    std::string str()
    {
        const U32 n = u32();
        if (!ok || n > 256 || p + n > d.size()) return ok = false, std::string();
        std::string s(reinterpret_cast<const char*>(d.data() + p), n);
        p += n;
        return s;
    }
};

} // namespace

// The save's clock and place, from its save.save; false if the file can't be read as a save (the hash alone then).
bool read_save_key(const std::wstring& path, U64& clock, U32& place)
{
    clock = 0, place = 0;
    std::vector<unsigned char> file;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0x80 && sz.QuadPart < (64ll << 20)) {
            file.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            if (!ReadFile(h, file.data(), static_cast<DWORD>(file.size()), &got, nullptr) || got != file.size()) file.clear();
        }
        CloseHandle(h);
    }
    if (file.size() < 0x80 || std::memcmp(file.data(), "SAVE", 4) != 0) return false;
    const U16 version = static_cast<U16>(file[4] | file[5] << 8);
    const U32 unpacked = le32(&file[0x12]), payload = le32(&file[0x16]);
    const unsigned char* key = version == 2 ? kKeyV2 : version == 1 ? kKeyV1 : nullptr;
    if (!key || payload > file.size() - 0x80 || unpacked > (256u << 20)) return false;
    chacha20(key, &file[0x1A], &file[0x80], payload);
    std::vector<unsigned char> d;
    if (!lz4_block(&file[0x80], payload, d, unpacked)) return false;

    // The schema: +0x0E u16 tag, u16 0, u16 type count, the first type's name; per type its fields (name, type name,
    // u16 kind, u16 size, u32 aux), then the next type's name.
    Reader r{d, 0x12};
    const U16 types = r.u16();
    std::string name = r.str();
    int index = -1;
    std::vector<std::pair<std::string, U16>> fields; // (name, size) of TransformFieldSaveData
    for (U16 t = 0; r.ok && t < types; ++t) {
        const U16 n = r.u16();
        std::vector<std::pair<std::string, U16>> f;
        for (U16 i = 0; r.ok && i < n; ++i) {
            std::string fn = r.str();
            r.str();
            r.u16();
            const U16 size = r.u16();
            r.u32();
            f.push_back({fn, size});
        }
        if (name == "TransformFieldSaveData") index = t, fields = f;
        if (t + 1 < types) name = r.str();
    }
    if (!r.ok || index < 0 || index > 0xFFFF) return false;

    // The record is stored inside another object: its header "u16 1, u8 mask, u16 class, u8 0, 8 x FF, u32 offset";
    // the offset points just after itself, then u32 0 and the present fields in schema order. Exactly one per save.
    unsigned char pat[11] = {static_cast<unsigned char>(index), static_cast<unsigned char>(index >> 8), 0};
    std::memset(pat + 3, 0xFF, 8);
    size_t at = 0;
    int found = 0;
    for (size_t p = r.p; p + sizeof pat + 8 <= d.size(); ++p)
        if (d[p] == pat[0] && std::memcmp(&d[p], pat, sizeof pat) == 0) at = p, ++found;
    if (found != 1 || at < 3 || d[at - 3] != 1 || d[at - 2] != 0) return false;
    const unsigned mask = d[at - 1];
    if (le32(&d[at + 11]) != at + 15) return false;
    size_t p = at + 19;
    bool have_clock = false, have_place = false;
    for (size_t i = 0; i < fields.size() && i < 8; ++i) {
        if (!(mask >> i & 1)) continue;
        const auto& [fn, size] = fields[i];
        if (p + size > d.size()) return false;
        if (fn == "_fieldTimeRaw" && size == 8) std::memcpy(&clock, &d[p], 8), have_clock = true;
        if (fn == "_position" && size == 12) {
            U32 h = 2166136261u;
            for (int k = 0; k < 12; ++k) h = (h ^ d[p + k]) * 16777619u;
            place = h ? h : 1, have_place = true;
        }
        p += size;
    }
    if (!have_clock || !have_place || !clock) return clock = 0, place = 0, false;
    return true;
}

} // namespace om
