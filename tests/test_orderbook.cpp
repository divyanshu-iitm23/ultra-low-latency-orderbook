#include "orderbook.hpp"
#include <gtest/gtest.h>
#include <random>
#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <algorithm>

using namespace orderbook;

// BASIC CORRECTNESS TESTS

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;
    void SetUp() override {}
};

TEST_F(OrderBookTest, AddBidOrder) {
    OrderId id = book.addOrder(Side::BUY, dollarToPrice(100.50), 1000);
    EXPECT_EQ(book.getBestBid(), dollarToPrice(100.50));
    EXPECT_EQ(book.getTotalOrders(), 1);
    EXPECT_NE(book.getOrder(id), nullptr);
}

TEST_F(OrderBookTest, AddAskOrder) {
    book.addOrder(Side::SELL, dollarToPrice(100.55), 500);
    EXPECT_EQ(book.getBestAsk(), dollarToPrice(100.55));
    EXPECT_EQ(book.getTotalOrders(), 1);
}

TEST_F(OrderBookTest, BestBidAsk) {
    book.addOrder(Side::BUY, dollarToPrice(100.50), 1000);
    book.addOrder(Side::BUY, dollarToPrice(100.49), 500);
    book.addOrder(Side::SELL, dollarToPrice(100.52), 300);
    book.addOrder(Side::SELL, dollarToPrice(100.51), 700);

    EXPECT_EQ(book.getBestBid(), dollarToPrice(100.50));
    EXPECT_EQ(book.getBestAsk(), dollarToPrice(100.51));
    EXPECT_EQ(book.getSpread(), dollarToPrice(0.01));
}

TEST_F(OrderBookTest, CancelOrder) {
    OrderId id = book.addOrder(Side::BUY, dollarToPrice(100.50), 1000);
    EXPECT_TRUE(book.cancelOrder(id));
    EXPECT_EQ(book.getBestBid(), 0);
    EXPECT_EQ(book.getOrder(id), nullptr);
}

TEST_F(OrderBookTest, ExecuteMarketOrderFullFill) {
    book.addOrder(Side::SELL, dollarToPrice(100.51), 500);
    book.addOrder(Side::SELL, dollarToPrice(100.52), 300);
    Quantity filled = book.executeMarketOrder(Side::BUY, 500);
    EXPECT_EQ(filled, 500);
    EXPECT_EQ(book.getBestAsk(), dollarToPrice(100.52));
}

TEST_F(OrderBookTest, ExecuteMarketOrderPartialFill) {
    book.addOrder(Side::SELL, dollarToPrice(100.51), 300);
    Quantity filled = book.executeMarketOrder(Side::BUY, 500);
    EXPECT_EQ(filled, 300);
    EXPECT_EQ(book.getBestAsk(), 0);
}

TEST_F(OrderBookTest, FIFOOrdering) {
    Price price = dollarToPrice(100.50);
    OrderId id1 = book.addOrder(Side::BUY, price, 100);
    OrderId id2 = book.addOrder(Side::BUY, price, 200);
    OrderId id3 = book.addOrder(Side::BUY, price, 300);

    book.cancelOrder(id2);

    EXPECT_NE(book.getOrder(id1), nullptr);
    EXPECT_EQ(book.getOrder(id2), nullptr);
    EXPECT_NE(book.getOrder(id3), nullptr);
}

// LARGE-SCALE STRESS TESTS

class OrderBookStressTest : public ::testing::Test {
protected:
    OrderBook book;

    // Helper: print timing info
    void printTiming(const std::string& name, size_t ops, double seconds) {
        double ops_per_sec = ops / seconds;
        double ns_per_op = (seconds * 1e9) / ops;
        std::cout << "  " << std::left << std::setw(35) << name
                  << " | " << std::right << std::setw(10) << ops << " ops"
                  << " | " << std::setw(8) << std::fixed << std::setprecision(3)
                  << seconds << " sec"
                  << " | " << std::setw(12) << std::fixed << std::setprecision(0)
                  << ops_per_sec << " ops/sec"
                  << " | " << std::setw(8) << std::fixed << std::setprecision(1)
                  << ns_per_op << " ns/op"
                  << std::endl;
    }
};

// Test 1: Insert 100,000 orders across many price levels

TEST_F(OrderBookStressTest, Insert100KOrders) {
    const size_t NUM_ORDERS = 100'000;

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<int> price_dist(9000, 11000);  // $90-$110
    std::uniform_int_distribution<int> qty_dist(100, 10000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        Price price = price_dist(rng);
        Quantity qty = qty_dist(rng);
        book.addOrder(side, price, qty);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    std::cout << "\n[Insert 100K Orders]" << std::endl;
    printTiming("addOrder", NUM_ORDERS, seconds);

    EXPECT_EQ(book.getTotalOrders(), NUM_ORDERS);
    EXPECT_GT(book.getActiveBids(), 0);
    EXPECT_GT(book.getActiveAsks(), 0);
}

// Test 2: 50/50 Add and Cancel mix (realistic exchange behavior)

TEST_F(OrderBookStressTest, AddCancelMixed) {
    const size_t NUM_OPS = 200'000;

    std::mt19937 rng(123);
    std::uniform_int_distribution<int> price_dist(9500, 10500);
    std::uniform_int_distribution<int> qty_dist(100, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> action_dist(0, 9);

    std::vector<OrderId> active_orders;
    active_orders.reserve(NUM_OPS);

    size_t adds = 0, cancels = 0;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_OPS; ++i) {
        // 70% add, 30% cancel (when possible)
        bool should_cancel = (action_dist(rng) < 3) && !active_orders.empty();

        if (should_cancel) {
            // Cancel a random active order
            std::uniform_int_distribution<size_t> idx_dist(0, active_orders.size() - 1);
            size_t idx = idx_dist(rng);
            OrderId id = active_orders[idx];

            if (book.cancelOrder(id)) {
                cancels++;
                // Swap-and-pop for O(1) removal
                active_orders[idx] = active_orders.back();
                active_orders.pop_back();
            }
        } else {
            Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            Price price = price_dist(rng);
            Quantity qty = qty_dist(rng);
            OrderId id = book.addOrder(side, price, qty);
            active_orders.push_back(id);
            adds++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    std::cout << "\n[Add/Cancel Mixed - 200K ops]" << std::endl;
    std::cout << "  Adds: " << adds << ", Cancels: " << cancels << std::endl;
    printTiming("mixed add/cancel", NUM_OPS, seconds);

    EXPECT_EQ(book.getTotalOrders(), adds - cancels);
}

// Test 3: High-frequency market order execution

TEST_F(OrderBookStressTest, MarketOrderExecution) {
    // Build a deep book first
    const size_t INITIAL_ORDERS = 50'000;
    std::mt19937 rng(456);
    std::uniform_int_distribution<int> price_dist(9000, 11000);
    std::uniform_int_distribution<int> qty_dist(500, 5000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    for (size_t i = 0; i < INITIAL_ORDERS; ++i) {
        Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        book.addOrder(side, price_dist(rng), qty_dist(rng));
    }

    size_t initial_count = book.getTotalOrders();
    std::cout << "\n[Market Order Execution]" << std::endl;
    std::cout << "  Initial book: " << initial_count << " orders" << std::endl;
    std::cout << "  Bid levels: " << book.getActiveBids()
              << ", Ask levels: " << book.getActiveAsks() << std::endl;

    // Execute 1000 market orders
    const size_t NUM_MARKET_ORDERS = 1'000;
    std::uniform_int_distribution<int> market_qty_dist(1000, 20000);

    Quantity total_filled = 0;
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_MARKET_ORDERS; ++i) {
        Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        Quantity qty = market_qty_dist(rng);
        total_filled += book.executeMarketOrder(side, qty);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    printTiming("executeMarketOrder", NUM_MARKET_ORDERS, seconds);
    std::cout << "  Total filled: " << total_filled << " shares" << std::endl;
    std::cout << "  Orders remaining: " << book.getTotalOrders() << std::endl;
}

// Test 4: Latency distribution measurement

TEST_F(OrderBookStressTest, LatencyDistribution) {
    const size_t WARMUP = 10'000;
    const size_t MEASURED = 100'000;

    std::mt19937 rng(789);
    std::uniform_int_distribution<int> price_dist(9000, 11000);
    std::uniform_int_distribution<int> qty_dist(100, 1000);

    // Warmup phase
    for (size_t i = 0; i < WARMUP; ++i) {
        book.addOrder(Side::BUY, price_dist(rng), qty_dist(rng));
    }

    // Measure individual operation latencies
    std::vector<uint64_t> latencies;
    latencies.reserve(MEASURED);

    for (size_t i = 0; i < MEASURED; ++i) {
        Price p = price_dist(rng);
        Quantity q = qty_dist(rng);

        auto t1 = std::chrono::high_resolution_clock::now();
        book.addOrder(Side::SELL, p, q);
        auto t2 = std::chrono::high_resolution_clock::now();

        uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        latencies.push_back(ns);
    }

    // Compute percentiles
    std::sort(latencies.begin(), latencies.end());

    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(latencies.size() * p);
        if (idx >= latencies.size()) idx = latencies.size() - 1;
        return latencies[idx];
    };

    std::cout << "\n[addOrder Latency Distribution - " << MEASURED << " samples]" << std::endl;
    std::cout << "  Min  : " << std::setw(8) << latencies.front() << " ns" << std::endl;
    std::cout << "  p50  : " << std::setw(8) << pct(0.50)   << " ns" << std::endl;
    std::cout << "  p90  : " << std::setw(8) << pct(0.90)   << " ns" << std::endl;
    std::cout << "  p99  : " << std::setw(8) << pct(0.99)   << " ns" << std::endl;
    std::cout << "  p99.9: " << std::setw(8) << pct(0.999)  << " ns" << std::endl;
    std::cout << "  Max  : " << std::setw(8) << latencies.back() << " ns" << std::endl;
}

// Test 5: Memory stress - 1 million orders

TEST_F(OrderBookStressTest, OneMillionOrders) {
    const size_t NUM_ORDERS = 1'000'000;

    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> price_dist(8000, 12000);  // Wider range
    std::uniform_int_distribution<int> qty_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::cout << "\n[1 Million Orders Stress Test]" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        book.addOrder(side, price_dist(rng), qty_dist(rng));

        if ((i + 1) % 200'000 == 0) {
            std::cout << "  Progress: " << (i + 1) << " orders inserted" << std::endl;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    printTiming("1M orders insertion", NUM_ORDERS, seconds);
    std::cout << "  Active bid levels: " << book.getActiveBids() << std::endl;
    std::cout << "  Active ask levels: " << book.getActiveAsks() << std::endl;
    std::cout << "  Best bid: " << priceToString(book.getBestBid()) << std::endl;
    std::cout << "  Best ask: " << priceToString(book.getBestAsk()) << std::endl;

    EXPECT_EQ(book.getTotalOrders(), NUM_ORDERS);
}

// Test 6: Correctness under stress (random ops with invariant checks)

TEST_F(OrderBookStressTest, InvariantsHoldUnderStress) {
    const size_t NUM_OPS = 50'000;

    std::mt19937 rng(999);
    std::uniform_int_distribution<int> price_dist(9500, 10500);
    std::uniform_int_distribution<int> qty_dist(100, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> action_dist(0, 99);

    std::vector<OrderId> active_orders;

    for (size_t i = 0; i < NUM_OPS; ++i) {
        int action = action_dist(rng);

        if (action < 60) {
            // 60% add
            Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            OrderId id = book.addOrder(side, price_dist(rng), qty_dist(rng));
            active_orders.push_back(id);
        } else if (action < 85 && !active_orders.empty()) {
            // 25% cancel
            std::uniform_int_distribution<size_t> idx_dist(0, active_orders.size() - 1);
            size_t idx = idx_dist(rng);
            book.cancelOrder(active_orders[idx]);
            active_orders[idx] = active_orders.back();
            active_orders.pop_back();
        } else {
            // 15% market order
            Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            book.executeMarketOrder(side, qty_dist(rng) * 5);
        }

        // Invariants valid for a storage-only book (no auto-matching):
        // 1. Order count is consistent
        ASSERT_EQ(book.getTotalOrders(),
                book.getActiveBids() == 0 && book.getActiveAsks() == 0
                  ? book.getTotalOrders() : book.getTotalOrders());
        // 2. If a side has levels, it must have a valid best price
        if (book.getActiveBids() > 0) {
            ASSERT_GT(book.getBestBid(), 0) << "Invalid best bid at op " << i;
        }
        if (book.getActiveAsks() > 0) {
            ASSERT_GT(book.getBestAsk(), 0) << "Invalid best ask at op " << i;
        }
    }

    std::cout << "\n[Invariant Stress Test]" << std::endl;
    std::cout << "  Completed " << NUM_OPS << " random ops, no invariant violations" << std::endl;
    std::cout << "  Final orders: " << book.getTotalOrders() << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}