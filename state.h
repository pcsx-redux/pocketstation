#pragma once
#include <cstring>
#include <vector>
#include "types.h"

// Flat little-endian byte stream for PocketStation::saveState()/loadState(). Every field has a
// fixed width, so a state blob of one format version always has the same size.
namespace PSKState {

struct Writer {
    std::vector<u8> buf;
    void bytes(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void u8v(u8 v) { buf.push_back(v); }
    void flag(bool v) { buf.push_back(v ? 1 : 0); }
    void u16v(u16 v) { for (int i = 0; i < 2; i++) buf.push_back(u8(v >> (8 * i))); }
    void u32v(u32 v) { for (int i = 0; i < 4; i++) buf.push_back(u8(v >> (8 * i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; i++) buf.push_back(u8(v >> (8 * i))); }
};

struct Reader {
    const u8* p;
    usize left;
    bool ok = true;
    bool take(usize n) {
        if (!ok || left < n) return ok = false;
        left -= n;
        return true;
    }
    void bytes(void* dst, usize n) {
        if (!take(n)) return;
        std::memcpy(dst, p, n);
        p += n;
    }
    u8 u8v() {
        if (!take(1)) return 0;
        return *p++;
    }
    bool flag() { return u8v() != 0; }
    u64 uN(int n) {
        if (!take(n)) return 0;
        u64 v = 0;
        for (int i = 0; i < n; i++) v |= u64(p[i]) << (8 * i);
        p += n;
        return v;
    }
    u16 u16v() { return u16(uN(2)); }
    u32 u32v() { return u32(uN(4)); }
    u64 u64v() { return uN(8); }
};

}  // namespace PSKState
