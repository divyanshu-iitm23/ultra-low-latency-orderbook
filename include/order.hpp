#pragma once

#include "types.hpp"
#include <iostream>

namespace orderbook {

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    OrderType type;
    
    //internal LL
    Order* next;
    Order* prev;
    
    // constructor
    Order(OrderId id_, Side side_, Price price_, Quantity qty_, 
          Timestamp ts_ = 0, OrderType type_ = OrderType::LIMIT)
        : id(id_)
        , side(side_)
        , price(price_)
        , quantity(qty_)
        , timestamp(ts_)
        , type(type_)
        , next(nullptr)
        , prev(nullptr)
    {}
    
    // default constructor
    Order() 
        : id(0), side(Side::BUY), price(0), quantity(0), 
          timestamp(0), type(OrderType::LIMIT), next(nullptr), prev(nullptr) 
    {}
    
    // printing
    void print() const {
        std::cout << "Order{id=" << id 
                  << ", side=" << (side == Side::BUY ? "BUY" : "SELL")
                  << ", price=" << priceToString(price)
                  << ", qty=" << quantity 
                  << "}" << std::endl;
    }
};

} // namespace orderbook