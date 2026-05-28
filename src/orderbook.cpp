#include "orderbook.hpp"
#include <iostream>
#include <algorithm>

namespace orderbook {

OrderId OrderBook::addOrder(Side side, Price price, Quantity quantity) {
    // creating new order
    Order* order = new Order(next_order_id_++, side, price, quantity);
    
    // adding to lookup map
    orders_[order->id] = order;
    
    // adding to appropriate price level
    if (side == Side::BUY) {
        bids_[price].addOrder(order);
    } else {
        asks_[price].addOrder(order);
    }
    
    total_orders_++;
    
    return order->id;
}

bool OrderBook::cancelOrder(OrderId order_id) {
    // finding order
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return false;  // order not found
    }
    
    Order* order = it->second;
    Price price = order->price;
    Side side = order->side;
    
    // removing from price level
    if (side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            level_it->second.removeOrder(order);
            
            // Remove price level if empty
            if (level_it->second.isEmpty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            level_it->second.removeOrder(order);
            
            if (level_it->second.isEmpty()) {
                asks_.erase(level_it);
            }
        }
    }
    
    // removing from lookup
    orders_.erase(it);
    
    // free memory
    delete order;
    
    return true;
}

Quantity OrderBook::executeMarketOrder(Side side, Quantity quantity) {
    Quantity remaining = quantity;

    // lambda to avoid the type mismatch issue
    auto processLevel = [&](auto& levels) {
        while (remaining > 0 && !levels.empty()) {
            auto best_level_it = levels.begin();
            PriceLevel& level = best_level_it->second;

            Quantity fill_qty = std::min(remaining, level.getTotalQuantity());
            Quantity filled_at_level = 0;

            while (filled_at_level < fill_qty) {
                Order* head = level.getHead();
                if (!head) break;

                Quantity to_fill = std::min(head->quantity, fill_qty - filled_at_level);

                if (to_fill == head->quantity) {
                    OrderId filled_id = head->id;
                    cancelOrder(filled_id);
                } else {
                    level.reduceHeadQuantity(to_fill);
                }

                filled_at_level += to_fill;
            }

            remaining -= filled_at_level;

            if (!levels.empty() && levels.begin()->second.isEmpty()) {
                levels.erase(levels.begin());
            }
        }
    };

    // market BUY hits asks, market SELL hits bids
    if (side == Side::BUY) {
        processLevel(asks_);
    } else {
        processLevel(bids_);
    }

    return quantity - remaining;
}

bool OrderBook::modifyOrder(OrderId order_id, Quantity new_quantity) {
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return false;
    }
    
    Order* order = it->second;
    
    // modify : updating quantity only
    // in real, price changes require cancel + add for time priority
    // Quantity old_qty = order->quantity;
    order->quantity = new_quantity;
    
    // updating price level total
    Price price = order->price;
    if (order->side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            level_it->second.getTotalQuantity();  // just accessing for now
        }
    }
    
    return true;
}

Price OrderBook::getBestBid() const {
    if (bids_.empty()) return 0;
    return bids_.begin()->first;
}

Price OrderBook::getBestAsk() const {
    if (asks_.empty()) return 0;
    return asks_.begin()->first;
}

Price OrderBook::getSpread() const {
    if (bids_.empty() || asks_.empty()) return 0;
    return getBestAsk() - getBestBid();
}

const Order* OrderBook::getOrder(OrderId order_id) const {
    auto it = orders_.find(order_id);
    return (it != orders_.end()) ? it->second : nullptr;
}

void OrderBook::printBook(int depth) const {
    std::cout << "\n========== ORDER BOOK ==========" << std::endl;
    
    // Print asks (in reverse for visual clarity)
    std::cout << "\nASKS (Sell Orders):" << std::endl;
    int count = 0;
    for (auto it = asks_.rbegin(); it != asks_.rend() && count < depth; ++it, ++count) {
        std::cout << "  " << priceToString(it->first) << " | " 
                  << it->second.getTotalQuantity() << " shares | "
                  << it->second.getOrderCount() << " orders" << std::endl;
    }
    
    std::cout << "\n--- SPREAD: " << priceToString(getSpread()) << " ---" << std::endl;
    
    // Print bids
    std::cout << "\nBIDS (Buy Orders):" << std::endl;
    count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && count < depth; ++it, ++count) {
        std::cout << "  " << priceToString(it->first) << " | " 
                  << it->second.getTotalQuantity() << " shares | "
                  << it->second.getOrderCount() << " orders" << std::endl;
    }
    
    std::cout << "\n================================\n" << std::endl;
}

} // namespace orderbook