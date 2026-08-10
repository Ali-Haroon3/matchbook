// Correctness tests for the matching engine. Zero external dependencies:
// a tiny CHECK harness so the suite runs anywhere with a C++20 compiler
// (and cleanly under ASAN/UBSAN/TSAN in CI).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

#include "matchbook/itch_book_builder.hpp"
#include "matchbook/matching_engine.hpp"
#include "matchbook/mold_udp64.hpp"
#include "matchbook/spsc_ring.hpp"
#include "matchbook/level_bitmap.hpp"
#include "matchbook/strategy/rl_quoter.hpp"

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

static void test_modify_events() {
    // Cancel-replace (reprice) must emit on_cancel(old) then on_accept(new)
    // so a handler is never left with a resting order it was never told
    // about; an in-place amend-down emits neither.
    struct Log {
        std::vector<std::pair<char, OrderId>> ev;  // 'A'ccept / 'C'ancel
        void on_accept(OrderId id, Side, Price, Qty) { ev.push_back({'A', id}); }
        void on_trade(const Trade&) {}
        void on_cancel(OrderId id) { ev.push_back({'C', id}); }
        void on_reject(OrderId) {}
    };
    Log log;
    MatchingEngine<Log> e(1, 10000, log);

    OrderId id = e.submit_limit(Side::Buy, 100, 5);   // A(id)
    CHECK(log.ev.size() == 1 && log.ev[0].first == 'A');

    CHECK(e.modify(id, 100, 3));                       // amend down: no event
    CHECK(log.ev.size() == 1);

    CHECK(e.modify(id, 105, 3));                       // reprice: C(id), A(id)
    CHECK(log.ev.size() == 3);
    CHECK(log.ev[1].first == 'C' && log.ev[1].second == id);
    CHECK(log.ev[2].first == 'A' && log.ev[2].second == id);
    CHECK(e.best_bid() == 105 && e.open_orders() == 1);
}

static void test_book_builder_replace_reject() {
    // A rejected Replace (out-of-band price) must leave the order reachable
    // under its old ref, not erase the mapping and orphan it in the engine.
    Recorder r;
    Engine e(1, 10000, r);
    itch::BookBuilder book;

    itch::Message add{};
    add.type = itch::MsgType::Add;
    add.ref = 999; add.side = Side::Buy; add.qty = 5; add.price = 100;
    book.apply(e, add);
    CHECK(e.best_bid() == 100 && e.open_orders() == 1);

    itch::Message bad{};
    bad.type = itch::MsgType::Replace;
    bad.ref = 999; bad.new_ref = 1000; bad.price = 999999; bad.qty = 5;
    book.apply(e, bad);                     // modify() rejects: out of band
    CHECK(e.open_orders() == 1 && e.best_bid() == 100);

    itch::Message del{};
    del.type = itch::MsgType::Delete;
    del.ref = 999;                          // old ref still resolves
    book.apply(e, del);
    CHECK(e.open_orders() == 0 && !e.has_bid());
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

static void test_post_only() {
    Recorder r;
    Engine e(1, 10000, r);
    e.submit_limit(Side::Sell, 100, 5);

    // Would cross (or even just lock) the ask: killed, book untouched.
    OrderId kill = e.submit_limit(Side::Buy, 100, 5, TimeInForce::PostOnly);
    CHECK(r.trades.empty());
    CHECK(r.cancels.size() == 1 && r.cancels[0] == kill);
    CHECK(!e.has_bid());
    CHECK(e.depth_at(Side::Sell, 100) == 5);
    CHECK(!e.cancel(kill));                    // killed order is gone
    OrderId thru = e.submit_limit(Side::Buy, 103, 5, TimeInForce::PostOnly);
    CHECK(r.cancels.back() == thru && !e.has_bid());

    // Passive price: rests exactly like GTC, cancellable, can later trade.
    OrderId rest = e.submit_limit(Side::Buy, 99, 7, TimeInForce::PostOnly);
    CHECK(e.has_bid() && e.best_bid() == 99);
    CHECK(e.depth_at(Side::Buy, 99) == 7);
    e.submit_limit(Side::Sell, 99, 7);
    CHECK(r.trades.size() == 1 && r.trades[0].maker == rest);

    // Empty opposite side: nothing to cross, rests.
    Recorder r2;
    Engine e2(1, 10000, r2);
    OrderId sell = e2.submit_limit(Side::Sell, 105, 3, TimeInForce::PostOnly);
    CHECK(e2.has_ask() && e2.best_ask() == 105);
    CHECK(e2.cancel(sell));
}

static void test_band_rejection() {
    Recorder r;
    Engine e(100, 200, r);
    CHECK(e.submit_limit(Side::Buy, 99, 5) == kInvalidOrderId);
    CHECK(e.submit_limit(Side::Buy, 201, 5) == kInvalidOrderId);
    CHECK(e.submit_limit(Side::Buy, 100, 5) != kInvalidOrderId);
    CHECK(e.submit_limit(Side::Sell, 200, 5) != kInvalidOrderId);
}

static void test_self_trade_prevention() {
    // CancelTaker (default): the incoming order stops dead at its own
    // resting order and its remainder is cancelled; the maker stays.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId mine = e.submit_limit(Side::Sell, 100, 5, TimeInForce::GTC, 7);
        OrderId taker = e.submit_limit(Side::Buy, 100, 8, TimeInForce::GTC, 7);
        CHECK(r.trades.empty());
        CHECK(r.cancels.size() == 1 && r.cancels[0] == taker);
        CHECK(e.depth_at(Side::Sell, 100) == 5);
        CHECK(!e.has_bid());                       // remainder must not rest
        CHECK(e.cancel(mine));
    }
    // Other people's orders ahead of mine still trade; the halt happens
    // only when the sweep reaches the same-owner order.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId theirs = e.submit_limit(Side::Sell, 100, 4, TimeInForce::GTC, 9);
        OrderId mine = e.submit_limit(Side::Sell, 100, 5, TimeInForce::GTC, 7);
        e.submit_limit(Side::Buy, 100, 10, TimeInForce::GTC, 7);
        CHECK(r.trades.size() == 1 && r.trades[0].maker == theirs);
        CHECK(r.trades[0].qty == 4);
        CHECK(e.depth_at(Side::Sell, 100) == 5);   // mine untouched
        CHECK(!e.has_bid());
        CHECK(e.cancel(mine));
    }
    // CancelMaker: my stale resting order is cancelled and the incoming
    // order keeps matching through it, across levels.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId mine = e.submit_limit(Side::Sell, 100, 5, TimeInForce::GTC, 7);
        OrderId theirs = e.submit_limit(Side::Sell, 101, 6, TimeInForce::GTC, 9);
        OrderId in = e.submit_limit(Side::Buy, 101, 6, TimeInForce::GTC, 7,
                                    StpPolicy::CancelMaker);
        CHECK(r.cancels.size() == 1 && r.cancels[0] == mine);
        CHECK(r.trades.size() == 1 && r.trades[0].maker == theirs);
        CHECK(r.trades[0].qty == 6 && r.trades[0].taker == in);
        CHECK(!e.has_ask() && !e.has_bid());
        CHECK(e.open_orders() == 0);
    }
    // CancelBoth: resting order cancelled, incoming remainder cancelled.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId mine = e.submit_limit(Side::Sell, 100, 5, TimeInForce::GTC, 7);
        e.submit_limit(Side::Sell, 101, 6, TimeInForce::GTC, 9);
        OrderId in = e.submit_limit(Side::Buy, 101, 8, TimeInForce::GTC, 7,
                                    StpPolicy::CancelBoth);
        CHECK(r.trades.empty());
        CHECK(r.cancels.size() == 2);
        CHECK(r.cancels[0] == mine && r.cancels[1] == in);
        CHECK(e.depth_at(Side::Sell, 101) == 6);   // the stranger survives
        CHECK(!e.has_bid());
    }
    // Owner 0 never triggers STP -- anonymous flow can self-cross.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 5);
        e.submit_limit(Side::Buy, 100, 5);
        CHECK(r.trades.size() == 1);
    }
    // Market order + STP: stops at the own order, remainder reported
    // unfilled, book left intact behind the halt.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 3, TimeInForce::GTC, 9);
        e.submit_limit(Side::Sell, 101, 5, TimeInForce::GTC, 7);
        e.submit_limit(Side::Sell, 102, 5, TimeInForce::GTC, 9);
        Qty rem = e.submit_market(Side::Buy, 10, 7);
        CHECK(rem == 7);                           // 3 filled, then halt
        CHECK(r.trades.size() == 1 && r.trades[0].qty == 3);
        CHECK(e.depth_at(Side::Sell, 101) == 5);
        CHECK(e.depth_at(Side::Sell, 102) == 5);
    }
    // FOK whose feasibility was met only by own liquidity: STP halts the
    // fill mid-way and cancels the remainder (documented interaction).
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 4, TimeInForce::GTC, 9);
        e.submit_limit(Side::Sell, 101, 6, TimeInForce::GTC, 7);
        OrderId fok = e.submit_limit(Side::Buy, 101, 10, TimeInForce::FOK, 7);
        CHECK(r.trades.size() == 1 && r.trades[0].qty == 4);
        CHECK(r.cancels.back() == fok);
        CHECK(!e.has_bid());
    }
    // Cancel-replace keeps owner and policy: repricing my bid through my
    // own ask still refuses to self-trade.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ask = e.submit_limit(Side::Sell, 105, 5, TimeInForce::GTC, 7);
        OrderId bid = e.submit_limit(Side::Buy, 100, 5, TimeInForce::GTC, 7);
        CHECK(e.modify(bid, 105, 5));              // would cross own ask
        CHECK(r.trades.empty());
        CHECK(e.depth_at(Side::Sell, 105) == 5);   // maker untouched
        CHECK(!e.has_bid());                       // replaced bid was killed
        CHECK(e.cancel(ask));
    }
}

static void test_iceberg() {
    // Resting shape: display clip visible, reserve hidden, one order.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Sell, 100, 25, 10);
        CHECK(e.best_ask() == 100);
        CHECK(e.depth_at(Side::Sell, 100) == 10);
        CHECK(e.hidden_at(Side::Sell, 100) == 15);
        CHECK(e.order_count_at(Side::Sell, 100) == 1);
        auto top = e.top_levels(Side::Sell, 3);
        CHECK(top.size() == 1 && top[0].qty == 10);

        // A lone iceberg refills clip after clip against one taker:
        // 10 + 10 + 5, three prints against the same maker.
        e.submit_limit(Side::Buy, 100, 25);
        CHECK(r.trades.size() == 3);
        CHECK(r.trades[0].qty == 10 && r.trades[0].maker == ice);
        CHECK(r.trades[1].qty == 10 && r.trades[1].maker == ice);
        CHECK(r.trades[2].qty == 5 && r.trades[2].maker == ice);
        CHECK(!e.has_ask() && !e.has_bid());
        CHECK(e.open_orders() == 0);
    }
    // Replenished clips lose time priority: after the first clip fills,
    // the order behind at the level trades before the next clip.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Sell, 100, 10, 5);
        OrderId behind = e.submit_limit(Side::Sell, 100, 3);
        e.submit_limit(Side::Buy, 100, 10);
        CHECK(r.trades.size() == 3);
        CHECK(r.trades[0].maker == ice && r.trades[0].qty == 5);
        CHECK(r.trades[1].maker == behind && r.trades[1].qty == 3);
        CHECK(r.trades[2].maker == ice && r.trades[2].qty == 2);
        CHECK(e.depth_at(Side::Sell, 100) == 3);   // clip remainder shows
        CHECK(e.hidden_at(Side::Sell, 100) == 0);
        CHECK(e.order_count_at(Side::Sell, 100) == 1);
    }
    // Crossing on entry: aggressive part executes for full size, only the
    // remainder rests dark.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 5);
        e.submit_iceberg(Side::Buy, 100, 12, 4);
        CHECK(r.trades.size() == 1 && r.trades[0].qty == 5);
        CHECK(e.depth_at(Side::Buy, 100) == 4);
        CHECK(e.hidden_at(Side::Buy, 100) == 3);
    }
    // FOK feasibility counts hidden reserve.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_iceberg(Side::Sell, 100, 30, 5);
        OrderId fok = e.submit_limit(Side::Buy, 100, 25, TimeInForce::FOK);
        CHECK(r.trades.size() == 5);               // 5 clips of 5
        CHECK(r.cancels.empty());
        Qty sum = 0;
        for (auto& t : r.trades) { sum += t.qty; CHECK(t.taker == fok); }
        CHECK(sum == 25);
        CHECK(e.depth_at(Side::Sell, 100) == 5);   // 30 - 25 left showing
        CHECK(e.hidden_at(Side::Sell, 100) == 0);
    }
    // Cancel removes displayed and hidden alike.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Buy, 90, 40, 8);
        e.submit_limit(Side::Sell, 90, 3);          // nibble the clip
        CHECK(e.depth_at(Side::Buy, 90) == 5);
        CHECK(e.cancel(ice));
        CHECK(!e.has_bid());
        CHECK(e.hidden_at(Side::Buy, 90) == 0);
        CHECK(e.open_orders() == 0);
    }
    // reduce() shaves the reserve first, keeping the clip and priority.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Sell, 100, 20, 6);
        CHECK(e.reduce(ice, 8));                    // 12 left: 6 shown, 6 dark
        CHECK(e.depth_at(Side::Sell, 100) == 6);
        CHECK(e.hidden_at(Side::Sell, 100) == 6);
        CHECK(e.reduce(ice, 8));                    // 4 left: all shown
        CHECK(e.depth_at(Side::Sell, 100) == 4);
        CHECK(e.hidden_at(Side::Sell, 100) == 0);
        CHECK(e.reduce(ice, 4));                    // gone
        CHECK(!e.has_ask() && e.open_orders() == 0);
    }
    // modify(): qty means new total. Amend-down keeps priority; reprice
    // is cancel-replace but stays an iceberg with the same peak.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Sell, 100, 20, 6);
        OrderId behind = e.submit_limit(Side::Sell, 100, 5);
        CHECK(e.modify(ice, 100, 10));              // shave: 6 shown, 4 dark
        CHECK(e.depth_at(Side::Sell, 100) == 11);
        CHECK(e.hidden_at(Side::Sell, 100) == 4);
        e.submit_limit(Side::Buy, 100, 6);
        CHECK(r.trades[0].maker == ice);            // priority kept

        CHECK(e.modify(ice, 101, 9));               // reprice, still iceberg
        CHECK(e.depth_at(Side::Sell, 101) == 6);    // peak carried over
        CHECK(e.hidden_at(Side::Sell, 101) == 3);
        CHECK(e.depth_at(Side::Sell, 100) == 5);    // `behind` stayed
        CHECK(e.cancel(ice) && e.cancel(behind));
    }
    // STP CancelMaker wipes a whole resting iceberg, hidden included.
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId ice = e.submit_iceberg(Side::Sell, 100, 30, 5, TimeInForce::GTC, 7);
        e.submit_limit(Side::Buy, 100, 3, TimeInForce::GTC, 7,
                       StpPolicy::CancelMaker);
        CHECK(r.trades.empty());
        CHECK(r.cancels.size() == 1 && r.cancels[0] == ice);
        CHECK(!e.has_ask());                        // iceberg fully gone
        CHECK(e.hidden_at(Side::Sell, 100) == 0);
        CHECK(e.has_bid() && e.depth_at(Side::Buy, 100) == 3);  // taker rests
        CHECK(e.open_orders() == 1);
    }
    // display >= total collapses to a plain limit: nothing hidden.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_iceberg(Side::Buy, 95, 7, 50);
        CHECK(e.depth_at(Side::Buy, 95) == 7);
        CHECK(e.hidden_at(Side::Buy, 95) == 0);
    }
    // Zero display or zero qty is rejected outright.
    {
        Recorder r;
        Engine e(1, 10000, r);
        CHECK(e.submit_iceberg(Side::Buy, 95, 10, 0) == kInvalidOrderId);
        CHECK(e.submit_iceberg(Side::Buy, 95, 0, 5) == kInvalidOrderId);
    }
}

static void test_stop_orders() {
    // A stop rests off-book until the tape reaches its trigger, then
    // executes as a market order.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 5);
        e.submit_limit(Side::Sell, 105, 5);
        OrderId stop = e.submit_stop(Side::Buy, 103, 5);
        CHECK(e.pending_stops() == 1);
        CHECK(e.stop_depth_at(Side::Buy, 103) == 5);
        CHECK(!e.has_bid());                        // invisible to the book
        CHECK(e.open_orders() == 3);                // 2 resting + 1 pending

        e.submit_limit(Side::Buy, 100, 5);          // prints 100 < 103: no fire
        CHECK(e.pending_stops() == 1);
        CHECK(r.trades.size() == 1);

        e.submit_limit(Side::Buy, 105, 5);          // prints 105 >= 103: fires
        CHECK(e.pending_stops() == 0);
        // The stop went off as a market order but the ask side is empty
        // now, so it discarded its quantity: trades are 100 and 105 only.
        CHECK(r.trades.size() == 2);
        CHECK(r.trades[1].price == 105);
        CHECK(stop != kInvalidOrderId);
        CHECK(!e.cancel(stop));                     // consumed, not resting
    }
    // Trigger-on-entry: the tape is already through the stop.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 5);
        e.submit_limit(Side::Buy, 100, 5);          // last = 100
        e.submit_limit(Side::Sell, 104, 7);
        OrderId stop = e.submit_stop(Side::Buy, 99, 4);   // 100 >= 99: fires now
        CHECK(e.pending_stops() == 0);
        CHECK(r.trades.size() == 2);
        CHECK(r.trades[1].taker == stop);
        CHECK(r.trades[1].price == 104 && r.trades[1].qty == 4);
    }
    // Stop-limit: fires into a limit; the remainder rests at the limit.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 100, 2);
        e.submit_limit(Side::Buy, 100, 2);          // last = 100
        e.submit_limit(Side::Sell, 104, 4);
        OrderId sl = e.submit_stop_limit(Side::Buy, 100, 105, 10);
        CHECK(r.trades.size() == 2);                // fired immediately
        CHECK(r.trades[1].taker == sl && r.trades[1].price == 104);
        CHECK(e.has_bid() && e.best_bid() == 105);  // 6 rested at the limit
        CHECK(e.depth_at(Side::Buy, 105) == 6);
        CHECK(e.cancel(sl));                        // now a normal resting order
    }
    // Cascade: one stop's fills arm the next; both fire in one pump.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 101, 5);
        e.submit_limit(Side::Sell, 102, 5);
        e.submit_limit(Side::Sell, 103, 5);
        OrderId s1 = e.submit_stop(Side::Buy, 101, 5);
        OrderId s2 = e.submit_stop(Side::Buy, 102, 5);
        CHECK(e.pending_stops() == 2);
        e.submit_limit(Side::Buy, 101, 5);          // print 101 arms s1
        // s1 lifts 102 (last=102) arming s2; s2 lifts 103.
        CHECK(e.pending_stops() == 0);
        CHECK(r.trades.size() == 3);
        CHECK(r.trades[1].taker == s1 && r.trades[1].price == 102);
        CHECK(r.trades[2].taker == s2 && r.trades[2].price == 103);
        CHECK(!e.has_ask());
    }
    // Multiple armed stops fire in trigger order (buy stops ascending).
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Sell, 106, 1);
        e.submit_limit(Side::Sell, 110, 1);
        e.submit_limit(Side::Sell, 111, 1);
        OrderId hi = e.submit_stop(Side::Buy, 106, 1);
        OrderId lo = e.submit_stop(Side::Buy, 103, 1);
        e.submit_limit(Side::Buy, 106, 1);          // print 106 arms both
        CHECK(e.pending_stops() == 0);
        CHECK(r.trades.size() == 3);
        CHECK(r.trades[1].taker == lo);             // ascending: 103 first
        CHECK(r.trades[1].price == 110);
        CHECK(r.trades[2].taker == hi);
        CHECK(r.trades[2].price == 111);
    }
    // Sell stops mirror: trigger at last <= stop, descending order.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_limit(Side::Buy, 95, 5);
        e.submit_limit(Side::Buy, 90, 5);
        OrderId stop = e.submit_stop(Side::Sell, 97, 5);
        e.submit_limit(Side::Buy, 96, 1);
        e.submit_limit(Side::Sell, 96, 1);          // print 96 <= 97: fires
        CHECK(e.pending_stops() == 0);
        CHECK(r.trades.size() == 2);
        CHECK(r.trades[1].taker == stop);
        CHECK(r.trades[1].price == 95 && r.trades[1].qty == 5);
    }
    // Pending stops can be cancelled (and only cancelled).
    {
        Recorder r;
        Engine e(1, 10000, r);
        OrderId stop = e.submit_stop_limit(Side::Sell, 90, 89, 5);
        CHECK(e.pending_stops() == 1);
        CHECK(!e.modify(stop, 91, 5));              // refuse modify
        CHECK(!e.reduce(stop, 1));                  // refuse reduce
        CHECK(e.cancel(stop));
        CHECK(e.pending_stops() == 0 && e.open_orders() == 0);
        CHECK(r.cancels.size() == 1 && r.cancels[0] == stop);
        CHECK(!e.cancel(stop));
        // modify to qty 0 is a cancel and is allowed on a pending stop.
        OrderId stop2 = e.submit_stop(Side::Sell, 90, 5);
        CHECK(e.modify(stop2, 90, 0));
        CHECK(e.pending_stops() == 0);
    }
    // Rejections: bad qty, out-of-band trigger or limit.
    {
        Recorder r;
        Engine e(100, 200, r);
        CHECK(e.submit_stop(Side::Buy, 150, 0) == kInvalidOrderId);
        CHECK(e.submit_stop(Side::Buy, 99, 5) == kInvalidOrderId);
        CHECK(e.submit_stop_limit(Side::Buy, 150, 201, 5) == kInvalidOrderId);
        CHECK(e.open_orders() == 0 && e.pending_stops() == 0);
    }
    // No trades yet: nothing can trigger, whatever the stop price.
    {
        Recorder r;
        Engine e(1, 10000, r);
        e.submit_stop(Side::Buy, 1, 5);             // would be instantly armed
        CHECK(e.pending_stops() == 1);              // ...if there were a tape
        CHECK(!e.has_last_trade());
    }
}

static void test_depth_snapshot() {
    Recorder r;
    Engine e(1, 10000, r);

    // Empty book: empty snapshot, zero counts.
    CHECK(e.top_levels(Side::Buy, 5).empty());
    CHECK(e.order_count_at(Side::Buy, 100) == 0);
    CHECK(e.order_count_at(Side::Buy, 999999) == 0);   // out of band

    e.submit_limit(Side::Buy, 100, 5);
    e.submit_limit(Side::Buy, 100, 7);
    e.submit_limit(Side::Buy, 98, 3);
    OrderId b96 = e.submit_limit(Side::Buy, 96, 9);
    e.submit_limit(Side::Sell, 105, 4);
    e.submit_limit(Side::Sell, 106, 6);

    // Bids best-first (descending), aggregated qty and per-level counts.
    auto bids = e.top_levels(Side::Buy, 10);
    CHECK(bids.size() == 3);
    CHECK(bids[0].price == 100 && bids[0].qty == 12 && bids[0].orders == 2);
    CHECK(bids[1].price == 98  && bids[1].qty == 3  && bids[1].orders == 1);
    CHECK(bids[2].price == 96  && bids[2].qty == 9  && bids[2].orders == 1);

    // Asks best-first (ascending); truncation honors max_levels.
    auto asks = e.top_levels(Side::Sell, 1);
    CHECK(asks.size() == 1);
    CHECK(asks[0].price == 105 && asks[0].qty == 4 && asks[0].orders == 1);

    // Counts stay consistent through fills and cancels.
    e.submit_limit(Side::Sell, 100, 4);                  // partial fill of first
    CHECK(e.order_count_at(Side::Buy, 100) == 2);        // both still resting
    e.submit_limit(Side::Sell, 100, 8);                  // finishes the level
    CHECK(e.order_count_at(Side::Buy, 100) == 0);
    CHECK(e.cancel(b96));
    CHECK(e.order_count_at(Side::Buy, 96) == 0);
    auto after = e.top_levels(Side::Buy, 10);
    CHECK(after.size() == 1 && after[0].price == 98);

    // visit_levels is the zero-alloc primitive under top_levels.
    size_t seen = 0;
    e.visit_levels(Side::Sell, 100, [&](const LevelView& v) {
        ++seen;
        CHECK(v.orders == 1);
    });
    CHECK(seen == 2);
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

static void test_mold_udp64() {
    // Round trip: three blocks, per-message sequence numbers, payloads.
    std::vector<std::vector<uint8_t>> msgs = {
        {'a', 'b', 'c'}, {'d'}, {'e', 'f'}};
    std::vector<uint8_t> pkt;
    mold::encode("SESH", 7, msgs, pkt);

    mold::Header hdr;
    std::vector<uint64_t> seqs;
    std::vector<std::vector<uint8_t>> got;
    int64_t n = mold::decode(pkt.data(), pkt.size(), hdr, [&](
        uint64_t seq, const uint8_t* p, size_t len) {
        seqs.push_back(seq);
        got.emplace_back(p, p + len);
    });
    CHECK(n == 3);
    CHECK(std::memcmp(hdr.session, "SESH      ", 10) == 0);
    CHECK(hdr.seq == 7);
    CHECK(seqs == (std::vector<uint64_t>{7, 8, 9}));
    CHECK(got == msgs);

    // Heartbeat (empty packet) and end-of-session.
    pkt.clear();
    mold::encode("SESH", 10, {}, pkt);
    n = mold::decode(pkt.data(), pkt.size(), hdr,
                     [&](uint64_t, const uint8_t*, size_t) { CHECK(false); });
    CHECK(n == 0 && hdr.count == 0);
    pkt.clear();
    mold::encode_end("SESH", 10, pkt);
    n = mold::decode(pkt.data(), pkt.size(), hdr,
                     [&](uint64_t, const uint8_t*, size_t) { CHECK(false); });
    CHECK(n == 0 && hdr.count == mold::kEndOfSession);

    // Malformed: truncated header; block length running past the end.
    pkt.clear();
    mold::encode("SESH", 1, msgs, pkt);
    auto nop = [](uint64_t, const uint8_t*, size_t) {};
    CHECK(mold::decode(pkt.data(), 10, hdr, nop) == -1);
    CHECK(mold::decode(pkt.data(), pkt.size() - 1, hdr, nop) == -1);

    // Sequence tracking: in order, duplicate, overlap, gap.
    mold::SequenceTracker t;
    CHECK(t.on_packet(1, 3) == 0);   // 1..3 fresh
    CHECK(t.expected() == 4);
    CHECK(t.on_packet(1, 3) == 3);   // full duplicate, skip all
    CHECK(t.expected() == 4);
    CHECK(t.on_packet(3, 3) == 1);   // overlap: skip 3, apply 4..5
    CHECK(t.expected() == 6);
    CHECK(t.on_packet(9, 2) == 0);   // gap: 6..8 lost
    CHECK(t.gap_messages() == 3);
    CHECK(t.expected() == 11);
}

template <typename Eng>
static void stress_invariants(uint64_t seed) {
    // Randomized fuzz: after every op, best bid < best ask (no locked or
    // crossed book) and open-order accounting stays consistent. Run against
    // both the default and the deferred-event engine (same invariants hold).
    Recorder r;
    Eng e(1, 2000, r, 1 << 16);
    uint64_t s = seed;
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

static void test_stress_invariants() {
    stress_invariants<MatchingEngine<Recorder>>(0x9E3779B97F4A7C15ull);
    stress_invariants<ReentrantMatchingEngine<Recorder>>(0xD1B54A32D192ED03ull);
}

// Records the full ordered event tape so two engines can be compared.
struct Tape {
    std::vector<std::array<long, 4>> ev;  // {kind, id/maker, price, qty}
    void on_accept(OrderId id, Side, Price p, Qty q) {
        ev.push_back({'A', (long)id, (long)p, (long)q});
    }
    void on_trade(const Trade& t) {
        ev.push_back({'T', (long)t.maker, (long)t.price, (long)t.qty});
    }
    void on_cancel(OrderId id) { ev.push_back({'C', (long)id, 0, 0}); }
    void on_reject(OrderId id) { ev.push_back({'R', (long)id, 0, 0}); }
};

template <typename Eng>
static void parity_script(Eng& e) {
    e.submit_limit(Side::Sell, 100, 10);
    e.submit_limit(Side::Sell, 101, 5);
    e.submit_limit(Side::Buy, 100, 4);                     // partial fill
    e.submit_limit(Side::Buy, 102, 12);                    // multi-level sweep + rest
    e.submit_limit(Side::Buy, 100, 3, TimeInForce::IOC);   // IOC remainder cancel
    e.submit_limit(Side::Sell, 90, 3, TimeInForce::FOK);   // FOK against the resting bid
    e.submit_limit(Side::Sell, 95, 2, TimeInForce::PostOnly);  // would cross: killed
    e.submit_limit(Side::Sell, 200, 2, TimeInForce::PostOnly); // passive: rests
    e.submit_limit(Side::Sell, 96, 4, TimeInForce::GTC, 7);    // owned resting ask
    e.submit_limit(Side::Buy, 96, 6, TimeInForce::GTC, 7);     // STP: taker killed
    e.submit_limit(Side::Buy, 96, 6, TimeInForce::GTC, 8,
                   StpPolicy::CancelMaker);                    // different owner: trades
    e.submit_iceberg(Side::Sell, 97, 9, 4);                    // dark clips
    e.submit_limit(Side::Buy, 97, 7);                          // clip, requeue, clip
    e.submit_iceberg(Side::Buy, 60, 12, 5, TimeInForce::IOC);  // no cross: killed
    e.submit_stop(Side::Buy, 70, 2);                           // parks (last=50)
    e.submit_stop_limit(Side::Sell, 40, 39, 2);                // parks
    e.submit_limit(Side::Sell, 75, 3);
    e.submit_limit(Side::Buy, 75, 1);                          // print 75: buy stop fires
    e.submit_market(Side::Sell, 2);                            // may print lower
    OrderId x = e.submit_limit(Side::Buy, 98, 7);
    e.modify(x, 105, 7);                                   // reprice: cancel + accept + match
    e.cancel(x);
    e.submit_market(Side::Sell, 100);                      // sweep whatever bids remain
    e.submit_limit(Side::Sell, 50, 1);
    e.submit_market(Side::Buy, 1);
}

static void test_reentrant_engine() {
    // 1. Event parity: the deferred engine emits the exact same tape as the
    //    default engine for a handler that does not re-enter -- same events,
    //    same order, only the dispatch timing differs.
    Tape fast, deferred;
    MatchingEngine<Tape> ef(1, 10000, fast);
    ReentrantMatchingEngine<Tape> ed(1, 10000, deferred);
    parity_script(ef);
    parity_script(ed);
    CHECK(fast.ev.size() > 10);
    CHECK(fast.ev == deferred.ev);

    // 2. Safe re-entrancy: a handler that cancels the maker from inside
    //    on_trade -- the exact pattern that use-after-frees the default
    //    engine -- is well-defined here, because the cancel runs after
    //    matching completes against a consistent book.
    struct CancelMaker {
        ReentrantMatchingEngine<CancelMaker>* e = nullptr;
        OrderId maker = 0;
        int cancels_ok = 0;
        void on_accept(OrderId, Side, Price, Qty) {}
        void on_trade(const Trade& t) {
            if (t.maker == maker && e->cancel(maker)) ++cancels_ok;
        }
        void on_cancel(OrderId) {}
        void on_reject(OrderId) {}
    };
    CancelMaker cm;
    ReentrantMatchingEngine<CancelMaker> e1(1, 10000, cm);
    cm.e = &e1;
    cm.maker = e1.submit_limit(Side::Sell, 100, 10);  // rests
    e1.submit_limit(Side::Buy, 100, 4);               // partial fill -> cancel 6 remaining
    CHECK(cm.cancels_ok == 1);
    CHECK(e1.open_orders() == 0 && !e1.has_ask() && !e1.has_bid());

    // 3. Re-entrant submit: a handler that places a new order from inside
    //    on_trade -- it matches the consistent post-op book and rests.
    struct Refill {
        ReentrantMatchingEngine<Refill>* e = nullptr;
        bool armed = false;
        int refills = 0;
        void on_accept(OrderId, Side, Price, Qty) {}
        void on_trade(const Trade&) {
            if (armed) { armed = false; e->submit_limit(Side::Sell, 101, 5); ++refills; }
        }
        void on_cancel(OrderId) {}
        void on_reject(OrderId) {}
    };
    Refill rf;
    ReentrantMatchingEngine<Refill> e2(1, 10000, rf);
    rf.e = &e2; rf.armed = true;
    e2.submit_limit(Side::Sell, 100, 5);
    e2.submit_limit(Side::Buy, 100, 5);               // fills -> re-entrant rest at 101
    CHECK(rf.refills == 1);
    CHECK(e2.has_ask() && e2.best_ask() == 101 && e2.open_orders() == 1);

    // 4. Exception safety: a handler that throws mid-drain must not leave
    //    stale buffered events to be re-delivered on the next operation.
    struct ThrowOnce {
        bool armed = true;
        std::vector<char> tape;
        void on_accept(OrderId, Side, Price, Qty) {
            tape.push_back('A');
            if (armed) { armed = false; throw std::runtime_error("boom"); }
        }
        void on_trade(const Trade&) { tape.push_back('T'); }
        void on_cancel(OrderId) { tape.push_back('C'); }
        void on_reject(OrderId) { tape.push_back('R'); }
    };
    ThrowOnce t;
    ReentrantMatchingEngine<ThrowOnce> e3(1, 10000, t);
    bool threw = false;
    try { e3.submit_limit(Side::Buy, 100, 5); }   // rests, then on_accept throws
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    CHECK(t.tape.size() == 1);                     // one accept dispatched, then threw
    CHECK(e3.open_orders() == 1);                  // the order still rested
    t.tape.clear();
    // The next op must deliver only its own events (accept + trade), with no
    // stale replay of the first order's buffered accept.
    e3.submit_limit(Side::Sell, 100, 5);
    CHECK(t.tape.size() == 2);                     // exactly A,T -- not 3 with a stale A
    CHECK(e3.open_orders() == 0);
}

static void test_rl_quoter() {
    using strategy::RLQuoter;
    RLQuoter rl;

    // Inventory bucketing clamps at the extremes and is symmetric-ish
    // around flat (integer division truncates toward zero).
    CHECK(rl.bucket(0) == RLQuoter::kInvRange);
    CHECK(rl.bucket(49) == RLQuoter::kInvRange);
    CHECK(rl.bucket(-49) == RLQuoter::kInvRange);
    CHECK(rl.bucket(50) == RLQuoter::kInvRange + 1);
    CHECK(rl.bucket(-50) == RLQuoter::kInvRange - 1);
    CHECK(rl.bucket(1000000) == RLQuoter::kStates - 1);
    CHECK(rl.bucket(-1000000) == 0);

    // Quotes never cross themselves, whatever the action.
    for (long inv : {-500L, 0L, 500L}) {
        auto q = rl.quotes(20000.0, inv, 0.0);
        CHECK(q.ask > q.bid);
    }

    // One Q-learning step from a zero table: Q(s,a) = alpha * reward.
    strategy::RLParams p;
    RLQuoter fresh(p);
    fresh.quotes(20000.0, 0, 0.0);  // greedy on a zero table picks action 0
    fresh.learn(10.0, 0);
    CHECK(std::abs(fresh.q_value(fresh.bucket(0), 0) - p.alpha * 10.0) < 1e-12);
    // Second update bootstraps off the improved state value:
    // Q += alpha * (r + discount * maxQ(s') - Q)
    double q1 = fresh.q_value(fresh.bucket(0), 0);
    fresh.quotes(20000.0, 0, 0.0);
    fresh.learn(10.0, 0);
    double expect = q1 + p.alpha * (10.0 + p.discount * q1 - q1);
    CHECK(std::abs(fresh.q_value(fresh.bucket(0), 0) - expect) < 1e-12);
    // learn() before any quotes() is a no-op, not UB.
    RLQuoter idle;
    idle.learn(5.0, 0);
    CHECK(idle.q_value(idle.bucket(0), 0) == 0.0);
}

int main() {
    test_basic_match();
    test_price_time_priority();
    test_execution_at_maker_price();
    test_partial_fill_rests();
    test_market_order();
    test_cancel();
    test_modify_priority_semantics();
    test_modify_events();
    test_book_builder_replace_reject();
    test_modify_can_cross();
    test_ioc();
    test_fok();
    test_post_only();
    test_band_rejection();
    test_self_trade_prevention();
    test_iceberg();
    test_stop_orders();
    test_depth_snapshot();
    test_bitmap();
    test_spsc_ring();
    test_mold_udp64();
    test_stress_invariants();
    test_reentrant_engine();
    test_rl_quoter();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
