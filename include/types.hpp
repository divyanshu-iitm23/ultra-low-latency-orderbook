#pragma once

#include <cstdint>
#include <string>

namespace orderbook {

// Side of the order
enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

// Order type
enum class OrderType : uint8_t {
    LIMIT = 0,      // specify price and quantity both
    MARKET = 1,     // specify only quantity, execute at best available price, used by aggressive buyers/sellers
};

// for price representation i'm using fixed-point(integer)
// example : $100.50 = 10050 (if tick size is $0.01)
// this avoids floating-point issues
using Price = int64_t;

// quantity in shares
using Quantity = uint64_t;

// unique order identifier
using OrderId = uint64_t;

// timestamp
using Timestamp = uint64_t;

// converting price to string
inline std::string priceToString(Price price, int decimals = 2) {
    double divisor = 1.0;
    for (int i = 0; i < decimals; ++i) divisor *= 10.0;
    return std::to_string(price / divisor);
}

// converting dollars to price ticks
// example : dollarToPrice(100.50, 2) = 10050
inline Price dollarToPrice(double dollars, int decimals = 2) {
    Price multiplier = 1;
    for (int i = 0; i < decimals; ++i) multiplier *= 10;
    return static_cast<Price>(dollars * multiplier);
}

} // namespace orderbook