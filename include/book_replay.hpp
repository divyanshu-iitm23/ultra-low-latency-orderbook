#pragma once
#include "orderbook.hpp"
#include "itch_parser.hpp"
#include <cstring>
#include <cstdint>
namespace orderbook {
// Drives an OrderBook from an ITCH message stream for ONE symbol.
// Symbol filtering: Add carries the stock; D/X/E/U reference orders by id, so adding
// only target-symbol orders means D/X/E/U for other symbols naturally find nothing.
//
// Optimization: ITCH stock symbols are exactly 8 bytes. Instead of strcmp() on every
// Add (~13% of replay time in profiling), we pack the 8-byte symbol into a uint64_t
// once at construction and compare a single 64-bit integer per Add.
class BookReplay {
public:
    BookReplay(OrderBook& book, const char* symbol) : book_(book) {
        char s[8];
        std::memset(s, ' ', 8);                      // ITCH symbols are space-padded to 8
        size_t len = std::strlen(symbol);
        if (len > 8) len = 8;
        std::memcpy(s, symbol, len);                 // left-justified, space-filled
        std::memcpy(&target_, s, 8);                 // pack 8 bytes -> uint64_t (raw bytes)
    }
    void onMessage(const itch::Message& m) {
        switch (m.type) {
            case 'A': case 'F': onAdd(m);               break;
            case 'D':           onDelete(m.orderRef);   break;
            case 'X':           onReduce(m.orderRef, m.shares); break; // partial cancel
            case 'E': case 'C': onReduce(m.orderRef, m.shares); break; // execution
            case 'U':           onReplace(m);           break;
        }
        ++seen_;
    }
    size_t seen() const { return seen_; }
    size_t added() const { return added_; }
    size_t deleted() const { return deleted_; }
    size_t reduced() const { return reduced_; }
    size_t replaced() const { return replaced_; }
private:
    static Price toCents(uint32_t itchPrice) { return (Price)(itchPrice / 100); }
    // Compare the message's raw 8-byte symbol against the packed target as one uint64_t.
    bool isTarget(const itch::Message& m) const {
        uint64_t raw; std::memcpy(&raw, m.rawStock, 8);
        return raw == target_;
    }
    void onAdd(const itch::Message& m) {
        if (!isTarget(m)) return;
        Side side = (m.side == 'B') ? Side::BUY : Side::SELL;
        if (book_.addOrder(side, toCents(m.price), m.shares, m.orderRef)) ++added_;
    }
    void onDelete(uint64_t ref) {
        if (book_.getOrder(ref)) { book_.cancelOrder(ref); ++deleted_; }
    }
    void onReduce(uint64_t ref, uint32_t shares) {
        const Order* o = book_.getOrder(ref);
        if (!o) return;
        if ((uint64_t)o->quantity > shares) book_.modifyOrder(ref, o->quantity - shares);
        else                                book_.cancelOrder(ref);
        ++reduced_;
    }
    void onReplace(const itch::Message& m) {
        const Order* o = book_.getOrder(m.orderRef);
        if (!o) return;
        Side side = o->side;
        book_.cancelOrder(m.orderRef);
        if (book_.addOrder(side, toCents(m.price), m.shares, m.newOrderRef)) ++replaced_;
    }
    OrderBook& book_;
    uint64_t  target_;                               // packed 8-byte target symbol
    size_t seen_=0, added_=0, deleted_=0, reduced_=0, replaced_=0;
};
}