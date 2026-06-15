#include "orderbook.hpp"
#include <cstdio>
#include <vector>
using namespace orderbook;

static int fails = 0;
#define CHK(c, msg) do{ if(!(c)){ printf("FAIL: %s\n", (msg)); ++fails; } }while(0)

// assert the matching invariant: a matched book is NEVER crossed
static void notCrossed(OrderBook& b, const char* where){
    Price bb=b.getBestBid(), ba=b.getBestAsk();
    if (bb && ba) CHK(bb < ba, where);
}

int main(){
    OrderBook b(1, 100000, 1<<16);   // cents ladder

    // ---- resting book ----
    b.addOrder(Side::SELL, 1000, 100, 1);   // ask $10.00, 100   (id1)
    b.addOrder(Side::SELL, 1000,  50, 2);   // ask $10.00, 50    (id2, FIFO after id1)
    b.addOrder(Side::SELL, 1001, 200, 3);   // ask $10.01, 200   (id3)
    b.addOrder(Side::BUY,   999, 100, 4);   // bid $9.99, 100    (id4)
    CHK(b.getBestAsk()==1000 && b.getBestBid()==999, "initial best prices");
    notCrossed(b, "after resting book");

    // ---- 1) aggressive BUY 120 @ $10.00 : hits id1(100) then id2(20), rests nothing ----
    auto t1 = b.submitLimit(Side::BUY, 1000, 120, 100);
    CHK(t1.size()==2, "trade1: two fills");
    CHK(t1[0].makerId==1 && t1[0].quantity==100 && t1[0].price==1000, "trade1[0]: id1 x100 @1000 (FIFO head)");
    CHK(t1[1].makerId==2 && t1[1].quantity==20  && t1[1].price==1000, "trade1[1]: id2 x20  @1000");
    CHK(b.getOrder(1)==nullptr, "id1 fully filled -> gone");
    CHK(b.getOrder(2)&&b.getOrder(2)->quantity==30, "id2 partially filled -> 30 left, stays");
    CHK(b.getOrder(100)==nullptr, "taker 100 fully filled -> did not rest");
    CHK(b.getBestAsk()==1000, "best ask still $10.00 (id2 remains)");
    notCrossed(b, "after trade1");

    // ---- 2) aggressive BUY 250 @ $10.01 : sweeps id2(30)@1000 + id3(200)@1001, rests 20 @ $10.01 ----
    auto t2 = b.submitLimit(Side::BUY, 1001, 250, 101);
    CHK(t2.size()==2, "trade2: two fills across two levels");
    CHK(t2[0].makerId==2 && t2[0].quantity==30  && t2[0].price==1000, "trade2[0]: id2 x30 @1000 (price priority)");
    CHK(t2[1].makerId==3 && t2[1].quantity==200 && t2[1].price==1001, "trade2[1]: id3 x200 @1001");
    CHK(b.getOrder(2)==nullptr && b.getOrder(3)==nullptr, "id2,id3 fully filled -> gone");
    CHK(b.getActiveAsks()==0, "ask side now empty");
    CHK(b.getOrder(101)&&b.getOrder(101)->quantity==20, "taker 101 rested 20 as a bid");
    CHK(b.getBestBid()==1001, "best bid now $10.01 (rested remainder)");
    notCrossed(b, "after trade2");

    // ---- 3) aggressive SELL 60 @ $9.99 : hits id101(20)@1001 then id4(40)@999, rests nothing ----
    auto t3 = b.submitLimit(Side::SELL, 999, 60, 102);
    CHK(t3.size()==2, "trade3: two fills");
    CHK(t3[0].makerId==101 && t3[0].quantity==20 && t3[0].price==1001, "trade3[0]: id101 x20 @1001 (best bid first)");
    CHK(t3[1].makerId==4   && t3[1].quantity==40 && t3[1].price==999,  "trade3[1]: id4 x40 @999");
    CHK(b.getOrder(101)==nullptr, "id101 fully filled -> gone");
    CHK(b.getOrder(4)&&b.getOrder(4)->quantity==60, "id4 partially filled -> 60 left");
    CHK(b.getOrder(102)==nullptr, "taker 102 fully filled -> did not rest");
    CHK(b.getBestBid()==999 && b.getActiveAsks()==0, "book: bid $9.99 only");
    notCrossed(b, "after trade3");

    // ---- 4) non-crossing BUY 100 @ $9.50 : no asks to cross -> just rests, no trades ----
    auto t4 = b.submitLimit(Side::BUY, 950, 100, 103);
    CHK(t4.empty(), "trade4: non-crossing limit produces no trades");
    CHK(b.getOrder(103)&&b.getOrder(103)->quantity==100, "id103 rested at $9.50");
    CHK(b.getBestBid()==999, "best bid still $9.99 (id4 better than id103)");
    notCrossed(b, "after trade4");

    // ---- 5) MARKET SELL 200 : sweeps all bids (id4 60 @999, id103 100 @950), 40 unfilled & dropped ----
    auto t5 = b.submitMarket(Side::SELL, 200, 104);
    CHK(t5.size()==2, "trade5: market sweeps both bid levels");
    CHK(t5[0].makerId==4   && t5[0].quantity==60  && t5[0].price==999, "trade5[0]: id4 x60 @999");
    CHK(t5[1].makerId==103 && t5[1].quantity==100 && t5[1].price==950, "trade5[1]: id103 x100 @950");
    CHK(b.getActiveBids()==0 && b.getActiveAsks()==0, "book fully empty after market sweep");
    CHK(b.getOrder(104)==nullptr, "market order never rests");
    notCrossed(b, "after trade5");

    printf(fails==0 ? "\nMATCHING ENGINE OK\n"
                    : "\n%d FAILURES\n", fails);
    return fails ? 1 : 0;
}