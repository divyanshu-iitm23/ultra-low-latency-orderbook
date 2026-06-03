#pragma once

#include "types.hpp"
#include <iostream>

namespace orderbook {

struct alignas(64) Order {              // 64 bytes, one cache line
    // hot fields: touched on every add/ cancel/ match
    OrderId   id;          // 8
    Price     price;       // 8
    Quantity  quantity;    // 8
    Order*    next;        // 8
    Order*    prev;        // 8
    // cold fields: rarely on the hot path
    Timestamp timestamp;   // 8
    Side      side;        // 1
    OrderType type;        // 1
    // 6 bytes padding; alignas(64) rounds the whole struct to 64

    Order(OrderId id_, Side side_, Price price_, Quantity qty_,
          Timestamp ts_ = 0, OrderType type_ = OrderType::LIMIT)
        : id(id_), price(price_), quantity(qty_), next(nullptr), prev(nullptr),
          timestamp(ts_), side(side_), type(type_)
    {}

    Order()
        : id(0), price(0), quantity(0), next(nullptr), prev(nullptr),
          timestamp(0), side(Side::BUY), type(OrderType::LIMIT)
    {}

    void print() const {
        std::cout << "Order{id=" << id
                  << ", side=" << (side == Side::BUY ? "BUY" : "SELL")
                  << ", price=" << priceToString(price)
                  << ", qty=" << quantity << "}" << std::endl;
    }
};

static_assert(sizeof(Order)  == 64, "Order must be exactly one cache line");
static_assert(alignof(Order) == 64, "Order must be cache-line aligned");

} // namespace orderbook