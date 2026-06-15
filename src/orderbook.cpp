#include "orderbook.hpp"
#include <algorithm>
namespace orderbook {

OrderId OrderBook::addOrder(Side side, Price price, Quantity quantity, OrderId id) {
    if (!inRange(price)) return 0;
    int64_t idx = priceToIndex(price);
    Order* o = order_pool_.allocate(id, side, price, quantity);           // step1: construct an order in the pool (say chunk[0])
    orders_[o->id] = o;                                                              // step2: store pointer to that order in the map (id->chunk[0])
    if (side == Side::BUY) {
        bool wasEmpty = bid_levels_[idx].isEmpty();
        bid_levels_[idx].addOrder(o);                          // step3: link that order into the price level's order list (chunk[0] is now in the LL of bid_levels_[idx])
        if (wasEmpty) { setBit(bid_words_, idx); ++bid_count_;
            if (best_bid_idx_<0 || idx>best_bid_idx_) best_bid_idx_=idx; }
    } else {
        bool wasEmpty = ask_levels_[idx].isEmpty();
        ask_levels_[idx].addOrder(o);                              // step3: link that order into the price level's order list (chunk[0] is now in the LL of ask_levels_[idx])
        if (wasEmpty) { setBit(ask_words_, idx); ++ask_count_;
            if (best_ask_idx_<0 || idx<best_ask_idx_) best_ask_idx_=idx; }
    }
    ++total_orders_;
    return o->id;
}

bool OrderBook::cancelOrder(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order* o = it->second;
    int64_t idx = priceToIndex(o->price);
    if (o->side == Side::BUY) {
        bid_levels_[idx].removeOrder(o);                                                // remove from bid's price level LL
        if (bid_levels_[idx].isEmpty()) { clearBit(bid_words_, idx); --bid_count_;
            if (idx==best_bid_idx_) best_bid_idx_=highestSetBit(bid_words_); }
    } else {
        ask_levels_[idx].removeOrder(o);                                                    // remove from ask's price level LL
        if (ask_levels_[idx].isEmpty()) { clearBit(ask_words_, idx); --ask_count_;
            if (idx==best_ask_idx_) best_ask_idx_=lowestSetBit(ask_words_); }
    }
    orders_.erase(it);
    order_pool_.deallocate(o);                                  // remove from map
    --total_orders_;
    return true;
}

Quantity OrderBook::executeMarketOrder(Side side, Quantity quantity) {
    Quantity remaining = quantity;
    if (side == Side::BUY) {
        while (remaining>0 && best_ask_idx_>=0) {
            PriceLevel& lvl = ask_levels_[best_ask_idx_];
            Quantity fill = std::min(remaining, lvl.getTotalQuantity());
            Quantity filled = 0;
            while (filled < fill) {
                Order* h = lvl.getHead(); if (!h) break;
                Quantity tf = std::min(h->quantity, fill-filled);
                if (tf == h->quantity) { OrderId hid=h->id; cancelOrder(hid); }
                else lvl.reduceHeadQuantity(tf);
                filled += tf;
            }
            remaining -= filled;
            if (filled == 0) break;
        }
    } else {
        while (remaining>0 && best_bid_idx_>=0) {
            PriceLevel& lvl = bid_levels_[best_bid_idx_];
            Quantity fill = std::min(remaining, lvl.getTotalQuantity());
            Quantity filled = 0;
            while (filled < fill) {
                Order* h = lvl.getHead(); if (!h) break;
                Quantity tf = std::min(h->quantity, fill-filled);
                if (tf == h->quantity) { OrderId hid=h->id; cancelOrder(hid); }
                else lvl.reduceHeadQuantity(tf);
                filled += tf;
            }
            remaining -= filled;
            if (filled == 0) break;
        }
    }
    return quantity - remaining;
}

bool OrderBook::modifyOrder(OrderId id, Quantity new_quantity) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order* o = it->second;
    int64_t idx = priceToIndex(o->price);
    int64_t delta = (int64_t)new_quantity - (int64_t)o->quantity;
    o->quantity = new_quantity;
    if (o->side == Side::BUY) bid_levels_[idx].adjustTotalQuantity(delta);
    else                      ask_levels_[idx].adjustTotalQuantity(delta);
    return true;
}
}

// Matching engine
namespace orderbook {

Quantity OrderBook::match(Side side, Price limit, Quantity quantity, OrderId takerId,
                          std::vector<Trade>& trades) {
    Quantity rem = quantity;
    if (side == Side::BUY) {
        // Aggressive buy crosses asks from the lowest price up, while ask <= limit.
        while (rem > 0 && best_ask_idx_ >= 0) {
            Price ap = indexToPrice(best_ask_idx_);
            if (ap > limit) break;                       // no longer crosses
            PriceLevel& lvl = ask_levels_[best_ask_idx_];
            while (rem > 0) {
                Order* m = lvl.getHead();                 // FIFO: oldest resting first
                if (!m) break;                            // level emptied
                Quantity fill = std::min(rem, m->quantity);
                trades.push_back({ap, fill, m->id, takerId, side});
                rem -= fill;
                if (fill == m->quantity) {                // maker fully filled -> remove
                    OrderId mid = m->id;
                    cancelOrder(mid);                     // updates level/bitmap/best/pool
                } else {                                  // maker partially filled -> reduce
                    m->quantity -= fill;
                    lvl.adjustTotalQuantity(-(int64_t)fill);
                }                                         // (rem is now 0 in the else branch)
            }
        }
    } else {
        // Aggressive sell crosses bids from the highest price down, while bid >= limit.
        while (rem > 0 && best_bid_idx_ >= 0) {
            Price bp = indexToPrice(best_bid_idx_);
            if (bp < limit) break;
            PriceLevel& lvl = bid_levels_[best_bid_idx_];
            while (rem > 0) {
                Order* m = lvl.getHead();
                if (!m) break;
                Quantity fill = std::min(rem, m->quantity);
                trades.push_back({bp, fill, m->id, takerId, side});
                rem -= fill;
                if (fill == m->quantity) { OrderId mid = m->id; cancelOrder(mid); }
                else { m->quantity -= fill; lvl.adjustTotalQuantity(-(int64_t)fill); }
            }
        }
    }
    return rem;
}

std::vector<Trade> OrderBook::submitLimit(Side side, Price price, Quantity quantity, OrderId takerId) {
    std::vector<Trade> trades;
    Quantity rem = match(side, price, quantity, takerId, trades);
    if (rem > 0) addOrder(side, price, rem, takerId);    // rest the unfilled remainder
    return trades;
}

std::vector<Trade> OrderBook::submitMarket(Side side, Quantity quantity, OrderId takerId) {
    std::vector<Trade> trades;
    Price limit = (side == Side::BUY) ? std::numeric_limits<Price>::max()
                                      : std::numeric_limits<Price>::min();
    match(side, limit, quantity, takerId, trades);            // remainder dropped (never rests)
    return trades;
}

} // namespace orderbook