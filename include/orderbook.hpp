#pragma once

#include "price_level.hpp"
#include "object_pool.hpp"
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

namespace orderbook {

class OrderBook {
private:
    // price levels : price -> PriceLevel
    // for bids : use reverse order (higher price = better)
    // for asks : use normal order (lower price = better)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;  // descending
    std::map<Price, PriceLevel> asks_;                        // ascending
    
    // order lookup: order_id -> Order pointer
    std::unordered_map<OrderId, Order*> orders_;
    
    // order ID generator(exchange assigns this)
    OrderId next_order_id_;

    size_t total_orders_;
    ObjectPool<Order> order_pool_;
    
public:
    // Size the pool up front. Default 65536 orders; grows automatically if exceeded.
    explicit OrderBook(size_t initial_pool_capacity = 1 << 16)
        : next_order_id_(1)
        , total_orders_(0)
        , order_pool_(initial_pool_capacity)   // ← ADD
    {}

    ~OrderBook() = default;
    
    // adding a limit order
    OrderId addOrder(Side side, Price price, Quantity quantity);
    
    // cancelling an order by ID
    bool cancelOrder(OrderId order_id);
    
    // executing a market order(returns quantity filled)
    Quantity executeMarketOrder(Side side, Quantity quantity);
    
    // modifying order (changing quantity only)
    bool modifyOrder(OrderId order_id, Quantity new_quantity);
    
    // getting best bid/ask prices
    Price getBestBid() const;
    Price getBestAsk() const;
    
    // getting bid-ask spread
    Price getSpread() const;
    
    // getting order by ID
    const Order* getOrder(OrderId order_id) const;
    
    void printBook(int depth = 5) const;
    
    size_t getTotalOrders() const { return total_orders_; }
    size_t getActiveBids() const { return bids_.size(); }
    size_t getActiveAsks() const { return asks_.size(); }

private:
    // helper
    std::map<Price, PriceLevel, std::greater<Price>>& getBidLevels() { return bids_; }
    std::map<Price, PriceLevel>& getAskLevels() { return asks_; }
};

} // namespace orderbook