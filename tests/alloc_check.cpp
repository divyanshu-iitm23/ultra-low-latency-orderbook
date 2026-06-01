// tests/alloc_check.cpp
#include "orderbook.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>

static std::atomic<size_t> g_new_calls{0};
void* operator new(std::size_t n)        { g_new_calls++; return std::malloc(n); }
void  operator delete(void* p) noexcept  { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace orderbook;

int main() {
    OrderBook book(100000);

    // Warm up: create every price level we'll use (forces map node allocs now).
    for (Price p = 9000; p <= 11000; ++p) book.addOrder(Side::BUY, p, 1);

    // Measure: steady-state adds at already-existing price levels.
    size_t before = g_new_calls.load();
    for (int i = 0; i < 100000; ++i) {
        book.addOrder(Side::SELL, 9000 + (i % 2001), 100);
    }
    size_t after = g_new_calls.load();

    std::cout << "Heap allocations during 100k steady-state adds: "
              << (after - before) << "\n";
    return 0;
}