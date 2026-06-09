#pragma once
#include "price_level.hpp"
#include "object_pool.hpp"
#include <unordered_map>
#include <vector>
#include <cstdint>
namespace orderbook {

class OrderBook {
public:
    // Price ladder spans [min_price, max_price] in ticks. One allocation per side.
    OrderBook(Price min_price=1, Price max_price=200000, size_t pool_cap=1<<16)
        : min_price_(min_price), max_price_(max_price),
          num_levels_((size_t)(max_price-min_price+1)),
          bid_levels_(num_levels_), ask_levels_(num_levels_),
          bid_words_((num_levels_+63)/64, 0), ask_words_((num_levels_+63)/64, 0),
          best_bid_idx_(-1), best_ask_idx_(-1), bid_count_(0), ask_count_(0),
          next_order_id_(1), total_orders_(0), order_pool_(pool_cap) {
        for (size_t i=0;i<num_levels_;++i) {
            Price p = min_price_ + (Price)i;
            bid_levels_[i].setPrice(p); ask_levels_[i].setPrice(p);
        }
        orders_.reserve(pool_cap);
    }
    ~OrderBook()=default;

    // book will assign the id to the orders

    OrderId addOrder(Side side, Price price, Quantity quantity){
        return addOrder(side, price, quantity, next_order_id_++);
    }
    // market-data path : caller supplies the exchange order reference as the id
    OrderId addOrder(Side side, Price price, Quantity quantity, OrderId id);

    bool cancelOrder(OrderId id);
    Quantity executeMarketOrder(Side side, Quantity quantity);
    bool modifyOrder(OrderId id, Quantity new_quantity);

    Price getBestBid() const { return best_bid_idx_<0?0:indexToPrice(best_bid_idx_); }
    Price getBestAsk() const { return best_ask_idx_<0?0:indexToPrice(best_ask_idx_); }
    Price getSpread() const {
        if (best_bid_idx_<0||best_ask_idx_<0) return 0;
        return getBestAsk()-getBestBid();
    }
    const Order* getOrder(OrderId id) const {
        auto it=orders_.find(id); return it==orders_.end()?nullptr:it->second;
    }
    size_t getTotalOrders() const { return total_orders_; }
    size_t getActiveBids() const { return bid_count_; }
    size_t getActiveAsks() const { return ask_count_; }

    void printBook(int depth = 5) const;

private:
    int64_t priceToIndex(Price p) const { return (int64_t)(p-min_price_); }
    Price indexToPrice(int64_t i) const { return min_price_+(Price)i; }
    bool inRange(Price p) const { return p>=min_price_ && p<=max_price_; }
    static void setBit(std::vector<uint64_t>& w, int64_t i){ w[i>>6]|=(1ULL<<(i&63)); }
    static void clearBit(std::vector<uint64_t>& w, int64_t i){ w[i>>6]&=~(1ULL<<(i&63)); }
    int64_t highestSetBit(const std::vector<uint64_t>& w) const {
        for (int64_t wi=(int64_t)w.size()-1; wi>=0; --wi)
            if (w[wi]) return wi*64 + (63 - __builtin_clzll(w[wi]));
        return -1;
    }
    int64_t lowestSetBit(const std::vector<uint64_t>& w) const {
        for (size_t wi=0; wi<w.size(); ++wi)
            if (w[wi]) return (int64_t)wi*64 + __builtin_ctzll(w[wi]);
        return -1;
    }

    Price min_price_, max_price_; size_t num_levels_;
    std::vector<PriceLevel> bid_levels_, ask_levels_;
    std::vector<uint64_t> bid_words_, ask_words_;
    int64_t best_bid_idx_, best_ask_idx_;
    size_t bid_count_, ask_count_;
    std::unordered_map<OrderId, Order*> orders_;
    OrderId next_order_id_; size_t total_orders_;
    ObjectPool<Order> order_pool_;
};
}