#include "orderbook.hpp"
#include <iostream>
#include <iomanip>
#include <random>
#include <chrono>
#include <vector>
#include <string>

using namespace orderbook;

void printSection(const std::string& title) {
    std::cout << "\n";
    std::cout << "================================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "================================================================\n";
}

void printSubSection(const std::string& title) {
    std::cout << "\n--- " << title << " ---\n";
}

std::string formatNumber(size_t n) {
    std::string s = std::to_string(n);
    int insert_position = s.length() - 3;
    while (insert_position > 0) {
        s.insert(insert_position, ",");
        insert_position -= 3;
    }
    return s;
}

class Timer {
    std::chrono::high_resolution_clock::time_point start_;
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}

    double elapsedSeconds() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }

    double elapsedMillis() const {
        return elapsedSeconds() * 1000.0;
    }
};

void demo1_basicVisualization() {
    printSection("DEMO 1: Basic Order Book Visualization");

    OrderBook book;

    std::cout << "Adding 6 limit orders...\n";

    book.addOrder(Side::BUY,  dollarToPrice(100.50), 1000);
    book.addOrder(Side::BUY,  dollarToPrice(100.49), 500);
    book.addOrder(Side::BUY,  dollarToPrice(100.48), 750);
    book.addOrder(Side::SELL, dollarToPrice(100.51), 600);
    book.addOrder(Side::SELL, dollarToPrice(100.52), 400);
    book.addOrder(Side::SELL, dollarToPrice(100.53), 1200);

    book.printBook(5);

    printSubSection("Executing market BUY for 800 shares");
    Quantity filled = book.executeMarketOrder(Side::BUY, 800);
    std::cout << "Filled: " << filled << " shares\n";

    book.printBook(5);
}

void demo2_realisticMarketSimulation() {
    printSection("DEMO 2: Realistic Market Simulation (10,000 orders)");

    OrderBook book;

    // Simulate stock trading around $100
    std::mt19937 rng(42);
    std::normal_distribution<double> price_dist(100.0, 0.50);  // Gaussian around $100
    std::uniform_int_distribution<int> qty_dist(100, 5000);
    std::uniform_int_distribution<int> side_dist(0, 1);

    const size_t NUM_ORDERS = 10'000;

    std::cout << "Simulating " << formatNumber(NUM_ORDERS)
              << " orders with prices clustered around $100.00 (Gaussian, sigma=$0.50)\n";

    Timer timer;
    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
        double price_dollars = price_dist(rng);
        Price price = dollarToPrice(price_dollars);
        Quantity qty = qty_dist(rng);
        book.addOrder(side, price, qty);
    }
    double elapsed = timer.elapsedMillis();

    std::cout << "\nCompleted in " << std::fixed << std::setprecision(2)
              << elapsed << " ms ("
              << std::fixed << std::setprecision(0)
              << (NUM_ORDERS / (elapsed / 1000.0)) << " orders/sec)\n";

    std::cout << "\nOrder book state:\n";
    std::cout << "  Total orders:    " << formatNumber(book.getTotalOrders()) << "\n";
    std::cout << "  Bid price levels: " << formatNumber(book.getActiveBids()) << "\n";
    std::cout << "  Ask price levels: " << formatNumber(book.getActiveAsks()) << "\n";
    std::cout << "  Best bid:        " << priceToString(book.getBestBid()) << "\n";
    std::cout << "  Best ask:        " << priceToString(book.getBestAsk()) << "\n";
    std::cout << "  Spread:          " << priceToString(book.getSpread()) << "\n";

    printSubSection("Top 10 levels of the book");
    book.printBook(10);
}

void demo3_millionOperations() {
    printSection("DEMO 3: One Million Operations Stress Test");

    OrderBook book;

    std::mt19937 rng(2024);
    std::uniform_int_distribution<int> price_dist(9500, 10500);
    std::uniform_int_distribution<int> qty_dist(100, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> action_dist(0, 99);

    std::vector<OrderId> active_orders;
    active_orders.reserve(1'000'000);

    const size_t NUM_OPS = 1'000'000;

    size_t adds = 0, cancels = 0, markets = 0;
    Quantity total_filled = 0;

    std::cout << "Running " << formatNumber(NUM_OPS) << " random operations...\n";
    std::cout << "Distribution: 70% add, 25% cancel, 5% market order\n\n";

    Timer timer;
    for (size_t i = 0; i < NUM_OPS; ++i) {
        int action = action_dist(rng);

        if (action < 70) {
            // Add
            Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            OrderId id = book.addOrder(side, price_dist(rng), qty_dist(rng));
            active_orders.push_back(id);
            adds++;
        } else if (action < 95 && !active_orders.empty()) {
            // Cancel
            std::uniform_int_distribution<size_t> idx_dist(0, active_orders.size() - 1);
            size_t idx = idx_dist(rng);
            if (book.cancelOrder(active_orders[idx])) {
                cancels++;
                active_orders[idx] = active_orders.back();
                active_orders.pop_back();
            }
        } else {
            // Market order
            Side side = (side_dist(rng) == 0) ? Side::BUY : Side::SELL;
            total_filled += book.executeMarketOrder(side, qty_dist(rng) * 3);
            markets++;
        }

        if ((i + 1) % 100'000 == 0) {
            std::cout << "  " << formatNumber(i + 1) << " ops completed... ("
                      << std::fixed << std::setprecision(1)
                      << timer.elapsedMillis() << " ms elapsed)\n";
        }
    }
    double elapsed = timer.elapsedMillis();

    std::cout << "\n--- Results ---\n";
    std::cout << "Total time:      " << std::fixed << std::setprecision(2)
              << elapsed << " ms\n";
    std::cout << "Throughput:      " << std::fixed << std::setprecision(0)
              << (NUM_OPS / (elapsed / 1000.0)) << " ops/sec\n";
    std::cout << "Avg latency:     " << std::fixed << std::setprecision(0)
              << ((elapsed * 1e6) / NUM_OPS) << " ns/op\n";
    std::cout << "\nOperation breakdown:\n";
    std::cout << "  Adds:          " << formatNumber(adds) << "\n";
    std::cout << "  Cancels:       " << formatNumber(cancels) << "\n";
    std::cout << "  Market orders: " << formatNumber(markets) << "\n";
    std::cout << "  Shares filled: " << formatNumber(total_filled) << "\n";
    std::cout << "\nFinal book state:\n";
    std::cout << "  Active orders:    " << formatNumber(book.getTotalOrders()) << "\n";
    std::cout << "  Bid levels:       " << formatNumber(book.getActiveBids()) << "\n";
    std::cout << "  Ask levels:       " << formatNumber(book.getActiveAsks()) << "\n";
    if (book.getActiveBids() > 0 && book.getActiveAsks() > 0) {
        std::cout << "  Best bid/ask:     " << priceToString(book.getBestBid())
                  << " / " << priceToString(book.getBestAsk()) << "\n";
        std::cout << "  Spread:           " << priceToString(book.getSpread()) << "\n";
    }
}

void demo4_burstSamePrice() {
    printSection("DEMO 4: Burst Test - 100K Orders at Same Price Level");

    OrderBook book;
    Price price = dollarToPrice(100.50);

    const size_t NUM_ORDERS = 100'000;

    std::cout << "Adding " << formatNumber(NUM_ORDERS)
              << " orders all at price " << priceToString(price)
              << "...\n";
    std::cout << "(Stress test for the intrusive linked list FIFO queue)\n\n";

    Timer timer;
    std::vector<OrderId> ids;
    ids.reserve(NUM_ORDERS);

    for (size_t i = 0; i < NUM_ORDERS; ++i) {
        ids.push_back(book.addOrder(Side::BUY, price, 100));
    }
    double add_time = timer.elapsedMillis();

    std::cout << "Insert time:  " << std::fixed << std::setprecision(2)
              << add_time << " ms ("
              << std::fixed << std::setprecision(0)
              << (add_time * 1e6 / NUM_ORDERS) << " ns/order)\n";
    std::cout << "Bid levels:   " << book.getActiveBids()
              << " (should be 1)\n";
    std::cout << "Orders at level: " << formatNumber(book.getTotalOrders()) << "\n";

    // Now cancel every other order (stress test for middle-of-list removal)
    printSubSection("Cancelling every other order (50K cancellations)");
    Timer cancel_timer;
    size_t cancelled = 0;
    for (size_t i = 0; i < NUM_ORDERS; i += 2) {
        if (book.cancelOrder(ids[i])) cancelled++;
    }
    double cancel_time = cancel_timer.elapsedMillis();

    std::cout << "Cancel time:  " << std::fixed << std::setprecision(2)
              << cancel_time << " ms ("
              << std::fixed << std::setprecision(0)
              << (cancel_time * 1e6 / cancelled) << " ns/cancel)\n";
    std::cout << "Cancelled:    " << formatNumber(cancelled) << "\n";
    std::cout << "Remaining:    " << formatNumber(book.getTotalOrders()) << "\n";
}

void demo5_sweepBook() {
    printSection("DEMO 5: Sweep Test - Massive Market Order Through Deep Book");

    OrderBook book;

    // Build a deep ask book with 1000 price levels, 100 orders each
    const size_t LEVELS = 1000;
    const size_t ORDERS_PER_LEVEL = 100;
    const Quantity QTY_PER_ORDER = 100;

    std::cout << "Building deep ask book: " << LEVELS << " price levels x "
              << ORDERS_PER_LEVEL << " orders = "
              << formatNumber(LEVELS * ORDERS_PER_LEVEL) << " total orders\n";

    Timer build_timer;
    for (size_t lvl = 0; lvl < LEVELS; ++lvl) {
        Price p = dollarToPrice(100.00) + lvl;  // 100.00, 100.01, 100.02, ...
        for (size_t i = 0; i < ORDERS_PER_LEVEL; ++i) {
            book.addOrder(Side::SELL, p, QTY_PER_ORDER);
        }
    }
    std::cout << "Build time: " << std::fixed << std::setprecision(2)
              << build_timer.elapsedMillis() << " ms\n";

    Quantity total_available = LEVELS * ORDERS_PER_LEVEL * QTY_PER_ORDER;
    std::cout << "Total liquidity: " << formatNumber(total_available) << " shares\n";

    printSubSection("Sweeping the entire book with one market BUY");

    Timer sweep_timer;
    Quantity filled = book.executeMarketOrder(Side::BUY, total_available);
    double sweep_time = sweep_timer.elapsedMillis();

    std::cout << "Sweep time:   " << std::fixed << std::setprecision(2)
              << sweep_time << " ms\n";
    std::cout << "Filled:       " << formatNumber(filled) << " shares\n";
    std::cout << "Per share:    " << std::fixed << std::setprecision(1)
              << (sweep_time * 1e6 / filled) << " ns/share\n";
    std::cout << "Book state:   " << book.getTotalOrders() << " orders remaining\n";
}

int main(int argc, char** argv) {
    std::cout << "================================================================\n";
    std::cout << "  ULTRA-LOW-LATENCY ORDER BOOK - MANUAL TEST SUITE\n";
    std::cout << "  Testing simple implementation with large datasets\n";
    std::cout << "================================================================\n";

    // Allow user to run specific demo via command line
    int demo = 0;  // 0 = run all
    if (argc > 1) {
        demo = std::atoi(argv[1]);
    }

    Timer total_timer;

    if (demo == 0 || demo == 1) demo1_basicVisualization();
    if (demo == 0 || demo == 2) demo2_realisticMarketSimulation();
    if (demo == 0 || demo == 3) demo3_millionOperations();
    if (demo == 0 || demo == 4) demo4_burstSamePrice();
    if (demo == 0 || demo == 5) demo5_sweepBook();

    printSection("ALL TESTS COMPLETED");
    std::cout << "Total runtime: " << std::fixed << std::setprecision(2)
              << total_timer.elapsedSeconds() << " seconds\n\n";

    return 0;
}