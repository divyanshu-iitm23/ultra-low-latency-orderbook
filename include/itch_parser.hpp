#pragma once
// itch_parser.hpp - NASDAQ TotalView-ITCH 5.0 message parser.
// Historical-sample framing: each message is preceded by a 2-byte big-endian length.
// ITCH is big-endian and PACKED (no alignment), so every field is read at an explicit
// offset and byte-swapped. Do NOT cast a struct over the bytes.
//
// NOTE: byte offsets below follow the Nasdaq TotalView-ITCH 5.0 spec. They MUST be
// validated against the official spec PDF and, ultimately, against a real sample file.
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace itch {

// --- big-endian field readers (network byte order) ---
inline uint16_t rdU16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint32_t rdU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
inline uint64_t rdU48(const uint8_t* p) {                 // 6-byte timestamp
    uint64_t v = 0; for (int i = 0; i < 6; ++i) v = (v << 8) | p[i]; return v;
}
inline uint64_t rdU64(const uint8_t* p) {
    uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v;
}

inline void trimStock(char* s) {            // s holds 8 chars + null; drop trailing spaces
    for (int i = 7; i >= 0; --i) { if (s[i] == ' ') s[i] = 0; else break; }
}

struct Message {
    char     type        = '?';
    uint64_t timestamp   = 0;   // ns since midnight (48-bit)
    uint64_t orderRef    = 0;
    uint64_t newOrderRef = 0;   // Replace ('U') only
    char     side        = 0;   // 'B' / 'S'  (Add only)
    uint32_t shares      = 0;
    uint32_t price       = 0;   // 1/10000 dollar (Add / Replace / ExecutedWithPrice)
    char     stock[9]    = {0}; // Add only; null-terminated, trailing spaces trimmed
    char     rawStock[8] = {0}; // Add only; raw 8 bytes (space-padded), for fast compare

    double priceDollars() const { return price / 10000.0; }
};

// Decode one message body (b[0] == type). Returns true for modeled order messages.
// Unmodeled/unknown types set .type (+ .timestamp when present) and return false.
inline bool decode(const uint8_t* b, size_t len, Message& m) {
    if (len < 1) return false;
    m = Message{};
    m.type = char(b[0]);
    switch (b[0]) {
        case 'A':                                    // Add Order (no MPID) - 36 bytes
            if (len < 36) return false;
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            m.side      = char(b[19]);
            m.shares    = rdU32(b + 20);
            std::memcpy(m.rawStock, b + 24, 8);
            std::memcpy(m.stock, b + 24, 8); trimStock(m.stock);
            m.price     = rdU32(b + 32);
            return true;
        case 'F':                                    // Add Order with MPID - 40 bytes
            if (len < 40) return false;              // first 36 bytes identical to 'A'
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            m.side      = char(b[19]);
            m.shares    = rdU32(b + 20);
            std::memcpy(m.rawStock, b + 24, 8);
            std::memcpy(m.stock, b + 24, 8); trimStock(m.stock);
            m.price     = rdU32(b + 32);
            return true;
        case 'E':                                    // Order Executed - 31 bytes
            if (len < 31) return false;
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            m.shares    = rdU32(b + 19);             // executed shares
            return true;
        case 'C':                                    // Order Executed With Price - 36 bytes
            if (len < 36) return false;
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            m.shares    = rdU32(b + 19);
            m.price     = rdU32(b + 32);
            return true;
        case 'X':                                    // Order Cancel (partial) - 23 bytes
            if (len < 23) return false;
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            m.shares    = rdU32(b + 19);             // cancelled shares
            return true;
        case 'D':                                    // Order Delete (full) - 19 bytes
            if (len < 19) return false;
            m.timestamp = rdU48(b + 5);
            m.orderRef  = rdU64(b + 11);
            return true;
        case 'U':                                    // Order Replace - 35 bytes
            if (len < 35) return false;
            m.timestamp   = rdU48(b + 5);
            m.orderRef    = rdU64(b + 11);           // original ref
            m.newOrderRef = rdU64(b + 19);           // new ref
            m.shares      = rdU32(b + 27);
            m.price       = rdU32(b + 31);
            return true;
        default:                                     // unmodeled: still skip correctly
            if (len >= 11) m.timestamp = rdU48(b + 5);
            return false;
    }
}

// Parse a buffer of framed ITCH (2-byte big-endian length prefix per message).
// Calls fn(const Message&) for each MODELED message. Returns count of modeled messages.
template <typename Fn>
size_t parseBuffer(const uint8_t* data, size_t size, Fn&& fn) {
    size_t off = 0, count = 0;
    while (off + 2 <= size) {
        uint16_t len = rdU16(data + off);
        if (len == 0) break;                         // padding / end
        if (off + 2 + size_t(len) > size) break;     // truncated tail
        Message m;
        if (decode(data + off + 2, len, m)) { fn(m); ++count; }
        off += 2 + len;                              // advance by framed length regardless
    }
    return count;
}

} // namespace itch