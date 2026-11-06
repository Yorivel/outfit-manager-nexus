// Safe memory access: reads and writes that survive bad pointers.
#pragma once
#include "om.h"

namespace om {

// --- Safe memory access -------------------------------------------------------------------------
inline bool plausible(U64 p) { return p > 0x10000 && p < kUserMax; }

template <class T>
bool rd(U64 addr, T& out)
{
    __try {
        out = *reinterpret_cast<volatile T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool rd_ptr(U64 addr, U64& out) { return rd(addr, out) && plausible(out); }

inline U64 rp(U64 addr)
{
    U64 v = 0;
    return rd_ptr(addr, v) ? v : 0;
}

inline U32 r32(U64 addr)
{
    U32 v = 0xFFFFFFFF;
    rd(addr, v);
    return v;
}

// Copies a C string from game memory (SEH-guarded).
inline std::string cstr(U64 p, size_t max = 120)
{
    std::string s;
    if (!plausible(p)) return s;
    for (size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!rd(p + i, c) || !c) break;
        s += c;
    }
    return s;
}

inline std::string hex(U64 p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::uint8_t v = 0;
        if (!rd(p + i, v)) return s + "??";
        std::snprintf(b, sizeof b, "%02x", v);
        if (i && i % 8 == 0) s += ' ';
        s += b;
    }
    return s;
}

template <class T>
bool wr(U64 addr, T v)
{
    __try {
        *reinterpret_cast<volatile T*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace om
