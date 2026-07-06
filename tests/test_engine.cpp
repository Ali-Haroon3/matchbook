// Correctness tests for the matching engine. Zero external dependencies:
// a tiny CHECK harness so the suite runs anywhere with a C++20 compiler
// (and cleanly under ASAN/UBSAN/TSAN in CI).
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "matchbook/matching_engine.hpp"
#include "matchbook/spsc_ring.hpp"
#include "matchbook/level_bitmap.hpp"

using namespace matchbook;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
        }                                                                  \
    } while (0)

struct Recorder {
    std::vector<Trade> trades;
    std::vector<OrderId> cancels;
    void on_accept(OrderId, Side, Price, Qty) {}
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_cancel(OrderId id) { cancels.push_back(id); }
    void on_reject(OrderId) {}
};

using Engine = MatchingEngine<Recorder>;

static void test_basic_match() {
    Recorder r;
    Engine e(1, 10000, r);
    OrderId sell = e.submit_limit(Side::Sell, 100, 10);
    CHECK(e.has_ask() && e.best_ask() == 100);
    OrderId buy = e.submit_limit(Side::Buy, 100, 10);
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].maker == sell);
    CHECK(r.trades[0].taker == buy);
    CHECK(r.trades[0].price == 100);
    CHECK(r.trades[0].qty == 10);
    CHECK(!e.has_ask() && !e.has_bid());
    CHECK(e.open_orders() == 0);
}

static void test_price_time_priority() {
    Recorder r;
    Engine e(1, 10000, r);
    OrderId a = e.submit_limit(Side::Sell, 101, 5);   // worse price
    OrderId b = e.submit_limit(Side::Sell, 100, 5);   // best price
    OrderId c = e.submit_limit(Side::Sell, 100, 5);   // same price, later
    e.submit_limit(Side::Buy, 101, 12);               // sweeps
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].maker == b);  // best price first
    CHECK(r.trades[1].maker == c);  // then FIFO at same price
    CHECK(r.trades[2].maker == a);  // then next level
    CHECK(r.trades[2].qty == 2);    // partial
    CHECK(e.depth_at(Side::Sell, 101) == 3);
}

static void test_execution_at_maker_price() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 10);
    e.submit_limit(Side::Buy, 105, 10);  // willing to pay 105
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].price == 100);     // executes at resting price
}

static void test_partial_fill_rests() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 4);
    OrderId buy = e.submit_limit(Side::Buy, 100, 10);
    CHECK(r.trades.size() == 1 && r.trades[0].qty == 4);
    CHECK(e.has_bid() && e.best_bid() == 100);
    CHECK(e.depth_at(Side::Buy, 100) == 6);
    // The rested remainder is a live order: it can be cancelled.
    CHECK(e.cancel(buy));
    CHECK(!e.has_bid());
}

static void test_market_order() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 5);
    e.submit_limit(Side::Sell, 102, 5);
    Qty rem = e.submit_market(Side::Buy, 12);
    CHECK(rem == 2);                      // book exhausted, rest discarded
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].price == 100 && r.trades[1].price == 102);
    CHECK(!e.has_ask());
    CHECK(!e.has_bid());                  // market never rests
}

static void test_cancel() {
    Recorder r;
    Engine e(1, 10000, r);
    OrderId a = e.submit_limit(Side::Buy, 100, 5);
    OrderId b = e.submit_limit(Side::Buy, 99, 5);
    CHECK(e.cancel(a));
    CHECK(!e.cancel(a));                  // double cancel fails
    CHECK(e.best_bid() == 99);            // best fell back via bitmap
    CHECK(e.cancel(b));
    CHECK(!e.has_bid());
    CHECK(e.cancel(999999) == false);     // unknown id
}

static void test_modify_priority_semantics() {
    Recorder r;
    Engine e(1, 10000, r);
    OrderId first  = e.submit_limit(Side::Sell, 100, 10);
    OrderId second = e.submit_limit(Side::Sell, 100, 10);

    // Amend down at the same price: keeps time priority.
    CHECK(e.modify(first, 100, 6));
    e.submit_limit(Side::Buy, 100, 6);
    CHECK(r.trades.back().maker == first);

    // Price change: cancel-replace, goes behind `second`.
    CHECK(e.modify(second, 100, 10) == true);  // no-op-ish amend path ok
    OrderId third = e.submit_limit(Side::Sell, 100, 10);
    CHECK(e.modify(second, 101, 10));          // move away and back
    CHECK(e.modify(second, 100, 10));
    r.trades.clear();
    e.submit_limit(Side::Buy, 100, 25);
    CHECK(r.trades.size() >= 2);
    CHECK(r.trades[0].maker == third);         // second lost its spot
    CHECK(r.trades[1].maker == second);
}

static void test_modify_can_cross() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 105, 5);
    OrderId bid = e.submit_limit(Side::Buy, 100, 5);
    CHECK(e.modify(bid, 105, 5));  // repriced through the ask -> trades
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0].taker == bid);
    CHECK(!e.has_ask() && !e.has_bid());
}

static void test_ioc() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 5);

    // Partial fill: remainder is cancelled, never rests.
    OrderId ioc = e.submit_limit(Side::Buy, 100, 8, TimeInForce::IOC);
    CHECK(r.trades.size() == 1 && r.trades[0].qty == 5);
    CHECK(!e.has_bid());
    CHECK(r.cancels.size() == 1 && r.cancels[0] == ioc);
    CHECK(e.open_orders() == 0);

    // No cross at all: nothing trades, nothing rests.
    OrderId miss = e.submit_limit(Side::Buy, 50, 3, TimeInForce::IOC);
    CHECK(r.trades.size() == 1);
    CHECK(!e.has_bid());
    CHECK(r.cancels.back() == miss);

    // Full fill: no cancel emitted.
    e.submit_limit(Side::Sell, 100, 5);
    r.cancels.clear();
    e.submit_limit(Side::Buy, 100, 5, TimeInForce::IOC);
    CHECK(r.trades.size() == 2);
    CHECK(r.cancels.empty());
    CHECK(!e.has_ask());
}

static void test_fok() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 5);
    e.submit_limit(Side::Sell, 102, 5);

    // Liquidity beyond the limit price doesn't count: killed, book intact.
    OrderId kill = e.submit_limit(Side::Buy, 101, 10, TimeInForce::FOK);
    CHECK(r.trades.empty());
    CHECK(r.cancels.size() == 1 && r.cancels[0] == kill);
    CHECK(e.depth_at(Side::Sell, 100) == 5);
    CHECK(e.depth_at(Side::Sell, 102) == 5);

    // Exactly enough across two levels: fills completely.
    e.submit_limit(Side::Buy, 102, 10, TimeInForce::FOK);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].price == 100 && r.trades[1].price == 102);
    CHECK(!e.has_ask());
    CHECK(e.open_orders() == 0);

    // Sell-side FOK against bids.
    e.submit_limit(Side::Buy, 100, 4);
    OrderId k2 = e.submit_limit(Side::Sell, 100, 5, TimeInForce::FOK);
    CHECK(r.cancels.back() == k2);
    CHECK(e.depth_at(Side::Buy, 100) == 4);
    e.submit_limit(Side::Sell, 100, 4, TimeInForce::FOK);
    CHECK(!e.has_bid());
}

static void test_band_rejection() {
    Recorder r;
    Engine e(100, 200, r);
    CHECK(e.submit_limit(Side::Buy, 99, 5) == kInvalidOrderId);
    CHECK(e.submit_limit(Side::Buy, 201, 5) == kInvalidOrderId);
    CHECK(e.submit_limit(Side::Buy, 100, 5) != kInvalidOrderId);
    CHECK(e.submit_limit(Side::Sell, 200, 5) != kInvalidOrderId);
}

static void test_bitmap() {
    LevelBitmap m(300);
    CHECK(m.find_le(299) == -1);
    CHECK(m.find_ge(0) == -1);
    m.set(5); m.set(64); m.set(200);
    CHECK(m.find_le(299) == 200);
    CHECK(m.find_le(199) == 64);
    CHECK(m.find_le(63) == 5);
    CHECK(m.find_le(4) == -1);
    CHECK(m.find_ge(0) == 5);
    CHECK(m.find_ge(6) == 64);
    CHECK(m.find_ge(65) == 200);
    CHECK(m.find_ge(201) == -1);
    m.clear(64);
    CHECK(m.find_ge(6) == 200);
}

static void test_spsc_ring() {
    SpscRing<int, 8> ring;
    int v = 0;
    CHECK(!ring.try_pop(v));                 // empty
    for (int i = 0; i < 8; ++i) CHECK(ring.try_push(i));
    CHECK(!ring.try_push(99));               // full
    for (int i = 0; i < 8; ++i) {
        CHECK(ring.try_pop(v));
        CHECK(v == i);                       // FIFO
    }
    CHECK(!ring.try_pop(v));
}

static void test_stress_invariants() {
    // Randomized fuzz: after every op, best bid < best ask (no locked or
    // crossed book) and open-order accounting stays consistent.
    Recorder r;
    Engine e(1, 2000, r, 1 << 16);
    uint64_t s = 0x9E3779B97F4A7C15ull;
    auto rng = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    std::vector<OrderId> live;
    for (int i = 0; i < 200000; ++i) {
        uint64_t roll = rng() % 100;
        Price px = 900 + static_cast<Price>(rng() % 200);
        if (roll < 55 || live.empty()) {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            OrderId id = e.submit_limit(side, px, 1 + rng() % 50);
            live.push_back(id);
        } else if (roll < 90) {
            size_t k = rng() % live.size();
            e.cancel(live[k]);  // may already be gone; that's the point
            live[k] = live.back();
            live.pop_back();
        } else if (roll < 95) {
            e.submit_market((rng() & 1) ? Side::Buy : Side::Sell,
                            1 + rng() % 100);
        } else {
            // IOC/FOK never rest, so their ids don't join `live`.
            e.submit_limit((rng() & 1) ? Side::Buy : Side::Sell, px,
                           1 + rng() % 50,
                           (roll & 1) ? TimeInForce::IOC : TimeInForce::FOK);
        }
        if (e.has_bid() && e.has_ask()) CHECK(e.best_bid() < e.best_ask());
    }
    for (OrderId id : live) e.cancel(id);
    CHECK(e.open_orders() == 0);
}

int main() {
    test_basic_match();
    test_price_time_priority();
    test_execution_at_maker_price();
    test_partial_fill_rests();
    test_market_order();
    test_cancel();
    test_modify_priority_semantics();
    test_modify_can_cross();
    test_ioc();
    test_fok();
    test_band_rejection();
    test_bitmap();
    test_spsc_ring();
    test_stress_invariants();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
