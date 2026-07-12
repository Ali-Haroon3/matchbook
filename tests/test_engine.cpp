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
    test_band_rejection();
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
