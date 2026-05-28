#pragma once

#include "order.hpp"
#include <iostream>

namespace orderbook {

class PriceLevel {
private:
    Price price_;
    Quantity total_quantity_;
    
    // intrusive doubly-LL of orders (FIFO)
    Order* head_;  // first order (oldest)
    Order* tail_;  // last order (newest)
    
    size_t order_count_;

public:
    PriceLevel(Price price = 0)
        : price_(price)
        , total_quantity_(0)
        , head_(nullptr)
        , tail_(nullptr)
        , order_count_(0)
    {}
    
    // getters
    Price getPrice() const { return price_; }
    Quantity getTotalQuantity() const { return total_quantity_; }
    size_t getOrderCount() const { return order_count_; }
    bool isEmpty() const { return head_ == nullptr; }
    Order* getHead() const { return head_; }
    
    // adding order to the end (newest)
    void addOrder(Order* order) {
        if (!order) return;
        
        order->next = nullptr;
        order->prev = tail_;
        
        if (tail_) {
            tail_->next = order;
        } else {
            // first order at this level
            head_ = order;
        }
        
        tail_ = order;
        total_quantity_ += order->quantity;
        order_count_++;
    }
    
    // remove specific order
    void removeOrder(Order* order) {
        if (!order) return;
        
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            // removing head
            head_ = order->next;
        }
        
        if (order->next) {
            order->next->prev = order->prev;
        } else {
            // removing tail
            tail_ = order->prev;
        }
        
        total_quantity_ -= order->quantity;
        order_count_--;
        
        order->next = nullptr;
        order->prev = nullptr;
    }
    
    // reduce quantity of the head order (for partial fills)
    void reduceHeadQuantity(Quantity qty) {
        if (!head_ || qty > head_->quantity) return;
        
        head_->quantity -= qty;
        total_quantity_ -= qty;
        
        // if it's fully filled we can remove it
        if (head_->quantity == 0) {
            Order* to_remove = head_;
            removeOrder(to_remove);
        }
    }
    
    // printing for debugging
    void print() const {
        std::cout << "PriceLevel{price=" << priceToString(price_)
                  << ", total_qty=" << total_quantity_
                  << ", orders=" << order_count_ << "}" << std::endl;
        
        Order* curr = head_;
        while (curr) {
            std::cout << "  ";
            curr->print();
            curr = curr->next;
        }
    }
};

} // namespace orderbook