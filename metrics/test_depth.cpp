// test_depth.cpp - deterministic test of OrderBook::getDepth().
#include "orderbook.hpp"
#include <cstdio>

using namespace orderbook;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); ++failures; } } while (0)

int main() {
    OrderBook book(1, 200000, 1u << 16);
    // bids at 99/100/101 (100 gets two orders -> aggregated qty), asks at 105/106
    book.addOrder(Side::BUY, 100, 10);
    book.addOrder(Side::BUY, 100,  5);   // same level -> total 15
    book.addOrder(Side::BUY, 101, 20);
    book.addOrder(Side::BUY,  99,  7);
    book.addOrder(Side::SELL, 105, 30);
    book.addOrder(Side::SELL, 106, 40);

    DepthLevel b[10], a[10]; int nb = 0, na = 0;
    book.getDepth(10, b, &nb, a, &na);

    REQUIRE(nb == 3, "3 bid levels");
    REQUIRE(b[0].price == 101 && b[0].qty == 20, "best bid is 101 x20 (highest first)");
    REQUIRE(b[1].price == 100 && b[1].qty == 15, "next bid 100 x15 (two orders aggregated)");
    REQUIRE(b[2].price ==  99 && b[2].qty ==  7, "deepest bid 99 x7");

    REQUIRE(na == 2, "2 ask levels");
    REQUIRE(a[0].price == 105 && a[0].qty == 30, "best ask is 105 x30 (lowest first)");
    REQUIRE(a[1].price == 106 && a[1].qty == 40, "next ask 106 x40");

    // maxLevels cap
    book.getDepth(2, b, &nb, a, &na);
    REQUIRE(nb == 2 && b[0].price == 101 && b[1].price == 100, "cap to top-2 bids, in order");
    REQUIRE(na == 2, "cap to top-2 asks");

    // a level that empties drops out of the ladder
    OrderBook b2(1, 200000, 1u << 16);
    OrderId id = b2.addOrder(Side::BUY, 100, 9);
    b2.addOrder(Side::SELL, 110, 3);
    b2.cancelOrder(id);                  // remove the only bid
    DepthLevel bb[10], aa[10]; int nbb = 0, naa = 0;
    b2.getDepth(10, bb, &nbb, aa, &naa);
    REQUIRE(nbb == 0, "no bids after the only bid level is cancelled");
    REQUIRE(naa == 1 && aa[0].price == 110, "ask side still has its level");

    printf(failures ? "\n%d FAILURES\n" : "\nDEPTH OK (top-N, aggregation, ordering, cap, emptying)\n",
           failures);
    return failures ? 1 : 0;
}
