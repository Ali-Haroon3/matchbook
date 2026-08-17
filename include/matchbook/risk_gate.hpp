#pragma once
// Pre-trade risk controls in front of a MatchingEngine: the market
// access layer (the shape SEC rule 15c3-5 mandates). The gate owns the
// engine, mirrors its order-entry surface, and refuses flow that would
// breach a limit *before* the engine sees it -- synchronously, by
// return value (kInvalidOrderId / untouched quantity), with the reason
// recorded. The wrapped handler observes the engine exactly as it
// would bare; gate rejects never reach it, engine rejects still do.
//
// Checks, in the order applied:
//   * kill switch          reject all new flow (kill_and_cancel_all()
//                          also flattens the book and the stop books)
//   * message budget       clockless: a counter per window, the caller
//                          decides what a window is via new_window()
//   * market orders        can be banned outright (allow_market)
//   * order quantity cap   per order
//   * notional cap         price x qty per order (market orders use the
//                          last trade as reference when there is one)
//   * price collar         |price - last trade| bound on any price that
//                          can execute: limits, icebergs, stop-limit
//                          limits, modify reprices. Stop-market
//                          triggers are exempt (they execute at market)
//   * open-order cap       engine-wide resting + pending stops
//   * position limit       per owner: |net filled qty +/- this order's
//                          worst-case fill| must stay inside the limit.
//                          Filled exposure only -- resting orders are
//                          not double-counted (per-order caps bound
//                          those); owner 0 is never position-checked
//
// Two rules are absolute, matching how real gates behave: risk
// *reduction* is never blocked (cancel and reduce pass every check,
// kill switch and exhausted budgets included), and admin operations
// (halt / resume / uncross / new_window) are not order flow and are
// never metered.
//
// Zero limits mean "unlimited": a default RiskLimits gate is a pure
// pass-through, which the tests pin down with an event-tape parity
// check against a bare engine.
#include <array>
#include <cstdint>
#include <vector>

#include "matching_engine.hpp"
#include "types.hpp"

namespace matchbook {

struct RiskLimits {
    Qty      max_order_qty   = 0;  // per order; 0 = unlimited
    int64_t  max_notional    = 0;  // price x qty per order; 0 = unlimited
    Price    price_collar    = 0;  // max |price - last trade|; 0 = off
    size_t   max_open_orders = 0;  // resting + pending stops; 0 = unlimited
    int64_t  max_position    = 0;  // per-owner net filled bound; 0 = off
    uint64_t max_messages    = 0;  // per window; 0 = unlimited
    bool     allow_market    = true;
};

enum class RiskReject : uint8_t {
    None = 0,
    Killed,
    MsgBudget,
    MarketBlocked,
    OrderQty,
    Notional,
    Collar,
    OpenOrders,
    Position,
};
inline constexpr size_t kRiskRejectKinds = 9;

template <typename Handler = NullHandler>
class RiskGate {
public:
    struct Sink;   // engine event handler, defined below

    RiskGate(Price min_price, Price max_price, Handler& user,
             RiskLimits limits = RiskLimits{},
             size_t expected_orders = 1 << 20)
        : user_(user),
          limits_(limits),
          engine_(min_price, max_price, sink_, expected_orders) {
        sink_.g = this;
        owner_of_.push_back(0);   // id 0 is invalid
    }

    // --- order entry (same signatures as the engine) ---------------------

    OrderId submit_limit(Side side, Price price, Qty qty,
                         TimeInForce tif = TimeInForce::GTC,
                         OwnerId owner = 0,
                         StpPolicy stp = StpPolicy::CancelTaker) {
        if (!admit(owner, side, qty, price, true)) return kInvalidOrderId;
        pending_owner_ = owner;
        return engine_.submit_limit(side, price, qty, tif, owner, stp);
    }
    Qty submit_market(Side side, Qty qty, OwnerId owner = 0,
                      StpPolicy stp = StpPolicy::CancelTaker) {
        if (!admit_market(owner, side, qty)) return qty;
        pending_owner_ = owner;
        return engine_.submit_market(side, qty, owner, stp);
    }
    OrderId submit_iceberg(Side side, Price price, Qty total_qty, Qty display,
                           TimeInForce tif = TimeInForce::GTC,
                           OwnerId owner = 0,
                           StpPolicy stp = StpPolicy::CancelTaker) {
        if (!admit(owner, side, total_qty, price, true)) return kInvalidOrderId;
        pending_owner_ = owner;
        return engine_.submit_iceberg(side, price, total_qty, display, tif,
                                      owner, stp);
    }
    OrderId submit_stop(Side side, Price stop_price, Qty qty,
                        OwnerId owner = 0,
                        StpPolicy stp = StpPolicy::CancelTaker) {
        // Stop-market: no executable price, so no notional/collar checks.
        if (!admit(owner, side, qty, 0, false)) return kInvalidOrderId;
        pending_owner_ = owner;
        return engine_.submit_stop(side, stop_price, qty, owner, stp);
    }
    OrderId submit_stop_limit(Side side, Price stop_price, Price limit_price,
                              Qty qty, OwnerId owner = 0,
                              StpPolicy stp = StpPolicy::CancelTaker) {
        if (!admit(owner, side, qty, limit_price, true))
            return kInvalidOrderId;
        pending_owner_ = owner;
        return engine_.submit_stop_limit(side, stop_price, limit_price, qty,
                                         owner, stp);
    }

    // Risk reduction is never blocked: cancel/reduce pass every check
    // (they still count against the message budget, they just cannot be
    // refused by it).
    bool cancel(OrderId id) {
        ++window_msgs_;
        return engine_.cancel(id);
    }
    bool reduce(OrderId id, Qty delta) {
        ++window_msgs_;
        return engine_.reduce(id, delta);
    }

    // A modify is new flow: re-checked against the kill switch, budget,
    // quantity, notional, and collar on its new terms. (Position is a
    // submit-time check; per-order caps bound a modify's exposure.)
    bool modify(OrderId id, Price new_price, Qty new_qty) {
        ++window_msgs_;
        if (killed_) return fail(RiskReject::Killed);
        if (over_budget()) return fail(RiskReject::MsgBudget);
        if (new_qty != 0) {
            if (!check_qty(new_qty)) return fail(RiskReject::OrderQty);
            if (!check_notional(new_price, new_qty))
                return fail(RiskReject::Notional);
            if (!check_collar(new_price)) return fail(RiskReject::Collar);
        }
        return engine_.modify(id, new_price, new_qty);
    }

    // --- session control (not order flow; never metered) -----------------

    void halt() { engine_.halt(); }
    void resume() { engine_.resume(); }
    bool is_halted() const noexcept { return engine_.is_halted(); }
    Qty uncross() { return engine_.uncross(); }

    // --- risk controls ----------------------------------------------------

    void set_limits(const RiskLimits& l) { limits_ = l; }
    const RiskLimits& limits() const noexcept { return limits_; }

    void new_window() noexcept { window_msgs_ = 0; }
    uint64_t window_messages() const noexcept { return window_msgs_; }

    void kill() noexcept { killed_ = true; }
    void clear_kill() noexcept { killed_ = false; }
    bool killed() const noexcept { return killed_; }

    // Throw the switch and flatten everything: every resting order and
    // every pending stop is cancelled (the handler sees the cancels).
    void kill_and_cancel_all() {
        killed_ = true;
        std::vector<OrderId> ids;
        ids.reserve(engine_.open_orders());
        engine_.visit_orders(Side::Buy, [&](OrderId id, Side, Price, Qty) {
            ids.push_back(id);
        });
        engine_.visit_orders(Side::Sell, [&](OrderId id, Side, Price, Qty) {
            ids.push_back(id);
        });
        engine_.visit_stops([&](OrderId id) { ids.push_back(id); });
        for (OrderId id : ids) engine_.cancel(id);
    }

    RiskReject last_reject() const noexcept { return last_reject_; }
    uint64_t rejects(RiskReject r) const noexcept {
        return reject_count_[static_cast<size_t>(r)];
    }
    // Net filled position (signed, buys positive) for a nonzero owner.
    int64_t position(OwnerId owner) const noexcept {
        return positions_[owner];
    }

    const MatchingEngine<Sink>& engine() const noexcept { return engine_; }

    // Engine event handler; forwards everything to the user handler and
    // keeps the gate's position book on the way through.
    struct Sink {
        RiskGate* g = nullptr;
        void on_accept(OrderId id, Side s, Price p, Qty q) {
            g->note_accept(id);
            g->user_.on_accept(id, s, p, q);
        }
        void on_trade(const Trade& t) {
            g->note_trade(t);
            g->user_.on_trade(t);
        }
        void on_cancel(OrderId id) { g->user_.on_cancel(id); }
        void on_reject(OrderId id) { g->user_.on_reject(id); }
        void on_rest(OrderId id, Side s, Price p, Qty q) {
            if constexpr (HasOnRest<Handler>) g->user_.on_rest(id, s, p, q);
            else { (void)id; (void)s; (void)p; (void)q; }
        }
    };

private:
    bool fail(RiskReject r) noexcept {
        last_reject_ = r;
        ++reject_count_[static_cast<size_t>(r)];
        return false;
    }

    bool over_budget() const noexcept {
        return limits_.max_messages != 0 &&
               window_msgs_ > limits_.max_messages;
    }
    bool check_qty(Qty q) const noexcept {
        return limits_.max_order_qty == 0 || q <= limits_.max_order_qty;
    }
    // Overflow-safe: qty <= cap / price instead of price * qty <= cap.
    bool check_notional(Price px, Qty q) const noexcept {
        if (limits_.max_notional == 0 || px <= 0) return true;
        return q <= static_cast<Qty>(limits_.max_notional / px);
    }
    bool check_collar(Price px) const noexcept {
        if (limits_.price_collar == 0 || !engine_.has_last_trade())
            return true;
        Price d = px - engine_.last_trade();
        if (d < 0) d = -d;
        return d <= limits_.price_collar;
    }
    bool check_open_orders() const noexcept {
        return limits_.max_open_orders == 0 ||
               engine_.open_orders() < limits_.max_open_orders;
    }
    bool check_position(OwnerId owner, Side side, Qty q) const noexcept {
        if (limits_.max_position == 0 || owner == 0) return true;
        int64_t worst = positions_[owner] +
                        ((side == Side::Buy) ? static_cast<int64_t>(q)
                                             : -static_cast<int64_t>(q));
        if (worst < 0) worst = -worst;
        return worst <= limits_.max_position;
    }

    // Shared admission ladder for priced flow (and, with checks_price
    // false, for stop-markets, which have no executable price).
    bool admit(OwnerId owner, Side side, Qty qty, Price px,
               bool checks_price) {
        ++window_msgs_;
        if (killed_) return fail(RiskReject::Killed);
        if (over_budget()) return fail(RiskReject::MsgBudget);
        if (!check_qty(qty)) return fail(RiskReject::OrderQty);
        if (checks_price) {
            if (!check_notional(px, qty)) return fail(RiskReject::Notional);
            if (!check_collar(px)) return fail(RiskReject::Collar);
        }
        if (!check_open_orders()) return fail(RiskReject::OpenOrders);
        if (!check_position(owner, side, qty))
            return fail(RiskReject::Position);
        return true;
    }

    bool admit_market(OwnerId owner, Side side, Qty qty) {
        ++window_msgs_;
        if (killed_) return fail(RiskReject::Killed);
        if (over_budget()) return fail(RiskReject::MsgBudget);
        if (!limits_.allow_market) return fail(RiskReject::MarketBlocked);
        if (!check_qty(qty)) return fail(RiskReject::OrderQty);
        // Market notional is judged against the last trade if the tape
        // has one (there is no order price to judge against).
        if (engine_.has_last_trade() &&
            !check_notional(engine_.last_trade(), qty))
            return fail(RiskReject::Notional);
        if (!check_open_orders()) return fail(RiskReject::OpenOrders);
        if (!check_position(owner, side, qty))
            return fail(RiskReject::Position);
        return true;
    }

    void note_accept(OrderId id) {
        if (owner_of_.size() <= id) owner_of_.resize(id + 1, 0);
        owner_of_[id] = pending_owner_;
    }
    void note_trade(const Trade& t) {
        int64_t q = static_cast<int64_t>(t.qty);
        int64_t taker_delta = (t.taker_side == Side::Buy) ? q : -q;
        OwnerId to = (t.taker < owner_of_.size()) ? owner_of_[t.taker] : 0;
        OwnerId mo = (t.maker < owner_of_.size()) ? owner_of_[t.maker] : 0;
        if (to != 0) positions_[to] += taker_delta;
        if (mo != 0) positions_[mo] -= taker_delta;
    }

    Handler& user_;
    RiskLimits limits_;
    Sink sink_;
    MatchingEngine<Sink> engine_;
    std::vector<OwnerId> owner_of_;             // engine id -> owner
    std::array<int64_t, 65536> positions_{};    // per-owner net filled
    std::array<uint64_t, kRiskRejectKinds> reject_count_{};
    uint64_t window_msgs_ = 0;
    OwnerId pending_owner_ = 0;
    RiskReject last_reject_ = RiskReject::None;
    bool killed_ = false;
};

}  // namespace matchbook
