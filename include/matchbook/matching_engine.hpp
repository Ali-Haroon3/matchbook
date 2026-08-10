#pragma once
// Price-time priority limit order book + matching engine.
//
// Design:
//   * Price levels live in a flat array indexed by tick offset from
//     min_price. O(1) level lookup, no tree, no hashing on price.
//   * Each level is an intrusive doubly-linked FIFO of Order nodes.
//     Time priority is list order; cancels unlink in O(1).
//   * Orders come from a chunked pool (pool.hpp): zero heap traffic on
//     the hot path, stable pointers.
//   * Best bid/ask are cached indices; when a best level empties, the
//     next best is recovered via a per-side occupancy bitmap
//     (level_bitmap.hpp) with ctz/clz word scans.
//   * OrderId -> Order* lookup is a flat vector (ids are engine-assigned
//     and sequential), so cancels/modifies are O(1) with one indirection.
//   * Event dispatch is a compile-time Handler policy: no virtuals,
//     no std::function.
//
// Semantics:
//   * submit_limit: matches immediately against the opposite side while
//     crossed, then rests any remainder. Time-in-force: GTC rests the
//     remainder, IOC cancels it, FOK executes fully or not at all,
//     PostOnly rests without ever taking (killed if it would cross).
//   * submit_market: matches until filled or the book is exhausted;
//     the remainder is discarded (never rests).
//   * cancel: O(1) removal by id.
//   * modify: qty decrease at the same price keeps time priority
//     (in-place amend); any price change or qty increase is treated as
//     cancel-replace and goes to the back of the queue, re-matching on
//     entry. This mirrors real exchange semantics (e.g. Nasdaq).
//   * self-trade prevention: submitting with a nonzero owner id arms STP
//     against resting orders with the same owner. The incoming order's
//     StpPolicy decides who yields: CancelTaker stops the match and kills
//     the incoming remainder, CancelMaker cancels the resting order and
//     keeps matching, CancelBoth does both. Owner 0 (default) disables
//     STP entirely, so the check is one predictable compare per fill.
//   * submit_iceberg: rests showing at most `display`; an exhausted clip
//     replenishes from the hidden reserve and re-queues at the back of
//     the level. Book depth reports displayed qty only; FOK feasibility
//     still counts hidden reserve. modify() on an iceberg interprets qty
//     as the new total and keeps the peak across a cancel-replace;
//     amend-down shaves the reserve before the displayed clip.
//   * submit_stop / submit_stop_limit: parked off-book until the last
//     trade price reaches the trigger (buy: last >= stop, sell: last <=
//     stop), then executed as a market order (remainder discarded) or a
//     GTC limit (remainder rests). Triggers are evaluated after every
//     operation; fills from one stop can arm the next (cascades run to
//     fixpoint, buy stops ascending then sell stops descending, FIFO
//     within a trigger level). Pending stops are invisible to depth and
//     best-price, cancellable by id, and refused by modify()/reduce().
//   * auctions: halt() suspends matching so GTC flow accumulates (the
//     book may cross); uncross() executes the overlap at one clearing
//     price -- max volume, then min imbalance, then nearest last trade,
//     then lowest -- and resume() returns to continuous trading, firing
//     any stops the auction prints armed. See the method comments.
//
// Event delivery (DeferEvents policy):
//   * false (default): the handler is called directly, mid-mutation, with
//     zero buffering -- the fast path. The engine is then NOT re-entrant: a
//     callback must not call back into submit_*/cancel/modify/reduce (a
//     debug-build assert enforces this).
//   * true (alias ReentrantMatchingEngine): events are buffered during the
//     operation and dispatched only after it completes, so callbacks run
//     against a fully consistent book and may freely re-enter the engine.
//     Costs one buffer push/pop per event, hence opt-in. Event order and
//     the "delivered before the call returns" timing are identical to the
//     default; only the safe re-entrancy and consistent-book view differ.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "level_bitmap.hpp"
#include "pool.hpp"
#include "types.hpp"

namespace matchbook {

// 48 bytes: owner/flags/ext ride in what used to be padding, so the node
// is no bigger than it was without them. Cold state for special order
// types (iceberg reserve, stop trigger data) lives in a side table
// reached through `ext`, keeping the plain-limit hot path untouched.
struct Order {
    OrderId id;
    Price   price;
    Qty     qty;      // remaining (icebergs: the *displayed* remainder)
    Order*  prev;
    Order*  next;
    int32_t ext;      // extras_ index, -1 for plain orders
    OwnerId owner;    // 0 = no self-trade prevention
    Side    side;
    uint8_t flags;    // bits 0-1: StpPolicy; bit 2: iceberg
};

static_assert(sizeof(Order) <= 48, "keep the order node hot-path small");

// One aggregated price level, as reported by top_levels()/visit_levels().
struct LevelView {
    Price    price;
    Qty      qty;     // total resting quantity at the level
    uint32_t orders;  // number of resting orders at the level
};

template <typename Handler = NullHandler, bool DeferEvents = false>
class MatchingEngine {
public:
    // Book covers prices in [min_price, max_price], inclusive, in ticks.
    MatchingEngine(Price min_price, Price max_price, Handler& handler,
                   size_t expected_orders = 1 << 20)
        : min_(min_price),
          n_levels_(static_cast<size_t>(max_price - min_price + 1)),
          bids_(n_levels_),
          asks_(n_levels_),
          bid_map_(n_levels_),
          ask_map_(n_levels_),
          pool_(expected_orders),
          h_(handler) {
        orders_.reserve(expected_orders);
        orders_.push_back(nullptr);  // id 0 is invalid
    }

    // --- order entry -----------------------------------------------------

    // Returns the assigned order id, or kInvalidOrderId if the price is
    // outside the book's band. IOC cancels any unfilled remainder instead
    // of resting it; FOK executes fully or not at all (killed via
    // on_cancel with no trades). A nonzero `owner` opts into self-trade
    // prevention under `stp` (see StpPolicy); note the FOK feasibility
    // pre-check counts own resting orders, so an FOK stopped by STP
    // mid-fill cancels its remainder like an IOC would.
    OrderId submit_limit(Side side, Price price, Qty qty,
                         TimeInForce tif = TimeInForce::GTC,
                         OwnerId owner = 0,
                         StpPolicy stp = StpPolicy::CancelTaker) {
        return run([&] {
            return do_submit_limit(side, price, qty, tif, owner, stp);
        });
    }

    // Returns unfilled quantity (0 if fully filled). An STP stop counts
    // the rest as unfilled (market remainders are discarded, never rest).
    Qty submit_market(Side side, Qty qty, OwnerId owner = 0,
                      StpPolicy stp = StpPolicy::CancelTaker) {
        return run([&] { return do_submit_market(side, qty, owner, stp); });
    }

    // Iceberg: rests showing at most `display`; every time the displayed
    // clip is exhausted by fills it is replenished from the hidden reserve
    // and re-queued at the back of the level, i.e. each clip has its own
    // time priority (exchange-standard). The full quantity is disclosed
    // only in this order's own on_accept; depth_at()/top_levels() see the
    // displayed clip. Hidden reserve still counts for FOK feasibility. If
    // the order crosses on entry, the aggressive part executes like a
    // plain limit for the full quantity; only the remainder rests dark.
    OrderId submit_iceberg(Side side, Price price, Qty total_qty, Qty display,
                           TimeInForce tif = TimeInForce::GTC,
                           OwnerId owner = 0,
                           StpPolicy stp = StpPolicy::CancelTaker) {
        return run([&] {
            return do_submit_iceberg(side, price, total_qty, display, tif,
                                     owner, stp);
        });
    }

    // Stop-market: parks off-book until the last trade price crosses the
    // trigger (buy stops arm at last >= stop_price, sell stops at
    // last <= stop_price), then executes as a market order. If the book
    // has already traded through the trigger on entry, it fires
    // immediately. Pending stops are invisible to depth/best-price, can
    // be cancelled by id, and are counted by open_orders()/pending_stops().
    OrderId submit_stop(Side side, Price stop_price, Qty qty,
                        OwnerId owner = 0,
                        StpPolicy stp = StpPolicy::CancelTaker) {
        return run([&] {
            return do_submit_stop(side, stop_price, 0, qty, true, owner, stp);
        });
    }

    // Stop-limit: as submit_stop, but on trigger it enters as a GTC limit
    // at `limit_price` (matching what it can, resting the remainder).
    OrderId submit_stop_limit(Side side, Price stop_price, Price limit_price,
                              Qty qty, OwnerId owner = 0,
                              StpPolicy stp = StpPolicy::CancelTaker) {
        return run([&] {
            return do_submit_stop(side, stop_price, limit_price, qty, false,
                                  owner, stp);
        });
    }

    bool cancel(OrderId id) {
        return run([&] { return cancel_impl(id); });
    }

    // Reduce a resting order's quantity by `delta` (feed-driven partial
    // execution or partial cancel). Keeps time priority; removes the order
    // if it reduces to zero. Returns false if the order is unknown.
    bool reduce(OrderId id, Qty delta) {
        return run([&] { return do_reduce(id, delta); });
    }

    // See semantics note at top of file.
    bool modify(OrderId id, Price new_price, Qty new_qty) {
        return run([&] { return do_modify(id, new_price, new_qty); });
    }

    // --- auction (call phase + uncross) ------------------------------------

    // Suspend continuous matching. While halted, GTC limits and icebergs
    // accumulate without matching (the book may cross), IOC/FOK/PostOnly
    // are killed on entry (they are continuous-session concepts), market
    // orders are rejected outright, and stops park without triggering.
    // cancel/modify/reduce work normally.
    void halt() {
        run([&] { halted_ = true; return 0; });
    }

    // Re-enable continuous matching. Does not uncross by itself: call
    // uncross() first for an auction, or resume directly and let any
    // standing overlap trade against new flow. Stops armed by auction
    // prints fire here, off the opening print, before this returns.
    void resume() {
        run([&] { halted_ = false; return 0; });
    }

    bool is_halted() const noexcept { return halted_; }

    // Cross the overlapped book at one clearing price and return the
    // executed quantity (0 if the book is not crossed). The price
    // maximizes executable volume; ties prefer the smallest leftover
    // imbalance, then the price nearest the last trade, then the lowest.
    // Fills follow price-time priority on both sides, print taker=buy /
    // maker=sell at the clearing price, and consume iceberg reserves
    // directly (hidden quantity participates in full, one print per
    // matched pair). Intended between halt() and resume(); STP is not
    // applied (an auction has no aggressor).
    Qty uncross() {
        return run([&] { return do_uncross(); });
    }

    // --- market data -----------------------------------------------------

    bool has_bid() const noexcept { return best_bid_ >= 0; }
    bool has_ask() const noexcept { return best_ask_ >= 0; }
    Price best_bid() const noexcept { return min_ + best_bid_; }
    Price best_ask() const noexcept { return min_ + best_ask_; }

    bool  has_last_trade() const noexcept { return has_last_; }
    Price last_trade() const noexcept { return last_px_; }

    // Pending (untriggered) stop orders, total and per trigger level.
    size_t pending_stops() const noexcept { return pending_stops_; }
    Qty stop_depth_at(Side side, Price stop_price) const noexcept {
        if (!in_band(stop_price) || stop_bids_.empty()) return 0;
        const auto& book = (side == Side::Buy) ? stop_bids_ : stop_asks_;
        return book[idx(stop_price)].total;
    }

    Qty depth_at(Side side, Price price) const noexcept {
        if (!in_band(price)) return 0;
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        return levels[idx(price)].total;
    }

    // Hidden (iceberg reserve) quantity at a level. Not part of the
    // published book view -- provided for analytics and tests.
    Qty hidden_at(Side side, Price price) const noexcept {
        if (!in_band(price)) return 0;
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        return levels[idx(price)].hidden;
    }

    uint32_t order_count_at(Side side, Price price) const noexcept {
        if (!in_band(price)) return 0;
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        return levels[idx(price)].count;
    }

    // Walk up to `max_levels` occupied levels best-first (bids descending,
    // asks ascending), calling f(LevelView) for each. Zero allocation: the
    // scan is bitmap hops over occupied levels only.
    template <typename F>
    void visit_levels(Side side, size_t max_levels, F&& f) const {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        int64_t i = (side == Side::Buy) ? best_bid_ : best_ask_;
        for (size_t n = 0; i >= 0 && n < max_levels; ++n) {
            const Level& lvl = levels[static_cast<size_t>(i)];
            f(LevelView{min_ + i, lvl.total, lvl.count});
            i = (side == Side::Buy) ? bid_map_.find_le(i - 1)
                                    : ask_map_.find_ge(i + 1);
        }
    }

    // Convenience snapshot of the top of the book (allocates).
    std::vector<LevelView> top_levels(Side side, size_t max_levels) const {
        std::vector<LevelView> out;
        out.reserve(max_levels);
        visit_levels(side, max_levels,
                     [&](const LevelView& v) { out.push_back(v); });
        return out;
    }

    size_t open_orders() const noexcept { return pool_.in_use(); }

private:
    struct Level {
        Qty      total  = 0;  // displayed resting quantity
        Qty      hidden = 0;  // iceberg reserve behind the displayed clips
        Order*   head   = nullptr;
        Order*   tail   = nullptr;
        uint32_t count  = 0;  // resting orders at this level
    };

    // Cold side-state for order types that outgrow the 48-byte node.
    struct Extra {
        Qty   reserve = 0;  // iceberg: hidden remainder
        Qty   peak    = 0;  // iceberg: clip size to replenish to
        Price limit   = 0;  // pending stop-limit: post-trigger limit price
    };

    // --- event delivery --------------------------------------------------

    // A buffered handler event (only materialized when DeferEvents).
    struct Event {
        enum class Kind : uint8_t { Accept, Trade, Cancel, Reject };
        Kind    kind;
        Trade   trade{};        // Trade
        OrderId id    = 0;      // Accept/Cancel/Reject
        Side    side  = Side::Buy;  // Accept
        Price   price = 0;      // Accept
        Qty     qty   = 0;      // Accept
    };

    void emit_accept(OrderId id, Side s, Price p, Qty q) {
        if constexpr (DeferEvents) {
            Event e; e.kind = Event::Kind::Accept;
            e.id = id; e.side = s; e.price = p; e.qty = q;
            queue_.push_back(e);
        } else {
            h_.on_accept(id, s, p, q);
        }
    }
    void emit_trade(const Trade& t) {
        if constexpr (DeferEvents) {
            Event e; e.kind = Event::Kind::Trade; e.trade = t;
            queue_.push_back(e);
        } else {
            h_.on_trade(t);
        }
    }
    void emit_cancel(OrderId id) {
        if constexpr (DeferEvents) {
            Event e; e.kind = Event::Kind::Cancel; e.id = id;
            queue_.push_back(e);
        } else {
            h_.on_cancel(id);
        }
    }
    void emit_reject(OrderId id) {
        if constexpr (DeferEvents) {
            Event e; e.kind = Event::Kind::Reject; e.id = id;
            queue_.push_back(e);
        } else {
            h_.on_reject(id);
        }
    }

    void dispatch(const Event& e) {
        switch (e.kind) {
            case Event::Kind::Accept: h_.on_accept(e.id, e.side, e.price, e.qty); break;
            case Event::Kind::Trade:  h_.on_trade(e.trade); break;
            case Event::Kind::Cancel: h_.on_cancel(e.id); break;
            case Event::Kind::Reject: h_.on_reject(e.id); break;
        }
    }

    // Drain buffered events after the outermost operation. A handler may
    // re-enter the engine here: the re-entrant call mutates the (now
    // consistent) book and appends its own events, which this same loop
    // then delivers in FIFO order. Each event is copied out before
    // dispatch, since a re-entrant append may reallocate the buffer.
    //
    // The buffer is emptied by the outermost Session's destructor, not here,
    // so it is cleared whether drain() returns normally or a handler throws
    // mid-dispatch. A throwing handler therefore aborts the rest of the
    // batch (those events are dropped, the throw propagates) but cannot
    // leave stale events to be re-delivered on the next call.
    void drain() {
        for (size_t i = 0; i < queue_.size(); ++i) {
            Event e = queue_[i];
            dispatch(e);
        }
    }

    // RAII scope wrapping every public entry point. Default mode: a
    // debug-only re-entrancy assert (nothing in release). Deferred mode:
    // tracks nesting depth so only the outermost call drains.
    struct Session {
        [[maybe_unused]] MatchingEngine& e;
        explicit Session([[maybe_unused]] MatchingEngine& eng) : e(eng) {
            if constexpr (DeferEvents) {
                ++e.depth_;
            }
#ifndef NDEBUG
            else {
                assert(e.depth_ == 0 &&
                       "MatchingEngine is not re-entrant in the default mode; "
                       "use ReentrantMatchingEngine to call back into the "
                       "engine from a Handler callback");
                e.depth_ = 1;
            }
#endif
        }
        ~Session() {
            if constexpr (DeferEvents) {
                // Emptying the buffer as the outermost call unwinds (not in
                // drain()) keeps it clean even if a handler threw mid-drain.
                if (--e.depth_ == 0) e.queue_.clear();
            }
#ifndef NDEBUG
            else {
                e.depth_ = 0;
            }
#endif
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        bool outermost() const noexcept { return e.depth_ == 1; }
    };

    // Run one public operation under a Session: the operation body, then
    // the stop-trigger pump (any fills just made may arm stops; their
    // executions may cascade), then (deferred mode only) event dispatch
    // once the outermost call unwinds. The pump itself submits through
    // the internal do_/place_ paths, so `pumping_` only guards against a
    // handler-driven re-entrant run() pumping concurrently in deferred
    // mode -- the outer pump's loop already covers those triggers.
    template <typename F>
    auto run(F&& f) {
        Session s(*this);
        auto result = f();
        if (!pumping_) {
            pumping_ = true;
            pump_stops();
            pumping_ = false;
        }
        if constexpr (DeferEvents) {
            if (s.outermost()) drain();
        }
        return result;
    }

    // --- operations ------------------------------------------------------

    OrderId do_submit_limit(Side side, Price price, Qty qty, TimeInForce tif,
                            OwnerId owner, StpPolicy stp) {
        if (!in_band(price) || qty == 0) [[unlikely]] {
            emit_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id();
        emit_accept(id, side, price, qty);
        if (halted_) [[unlikely]] {
            if (tif != TimeInForce::GTC) {
                emit_cancel(id);            // no continuous concepts in a call
                return id;
            }
            rest(id, side, price, qty, owner, stp);
            return id;
        }
        if (tif == TimeInForce::FOK && fillable(side, price, qty) < qty) {
            emit_cancel(id);
            return id;
        }
        if (tif == TimeInForce::PostOnly && would_cross(side, price)) {
            emit_cancel(id);
            return id;
        }
        place_limit(id, side, price, qty, tif, owner, stp);
        return id;
    }

    Qty do_submit_market(Side side, Qty qty, OwnerId owner, StpPolicy stp) {
        if (halted_) [[unlikely]] {
            emit_reject(kInvalidOrderId);   // no market orders in a call phase
            return qty;
        }
        OrderId id = next_id();
        emit_accept(id, side, 0, qty);
        Qty remaining = (side == Side::Buy)
            ? match_buy(id, qty, static_cast<int64_t>(n_levels_) - 1, owner, stp)
            : match_sell(id, qty, 0, owner, stp);
        stp_halt_ = false;  // market remainders are discarded either way
        return remaining;
    }

    OrderId do_submit_stop(Side side, Price stop_price, Price limit_price,
                           Qty qty, bool is_market, OwnerId owner,
                           StpPolicy stp) {
        if (!in_band(stop_price) || qty == 0 ||
            (!is_market && !in_band(limit_price))) [[unlikely]] {
            emit_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id();
        emit_accept(id, side, stop_price, qty);
        if (!halted_ && has_last_ && stop_triggered(side, stop_price, last_px_)) {
            // Already through the trigger: fire immediately. No second
            // accept -- the id was announced above; fills reference it.
            execute_stop(id, side, limit_price, qty, is_market, owner, stp);
            return id;
        }
        rest_stop(id, side, stop_price, limit_price, qty, is_market, owner,
                  stp);
        return id;
    }

    OrderId do_submit_iceberg(Side side, Price price, Qty total_qty,
                              Qty display, TimeInForce tif, OwnerId owner,
                              StpPolicy stp) {
        if (!in_band(price) || total_qty == 0 || display == 0) [[unlikely]] {
            emit_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id();
        emit_accept(id, side, price, total_qty);
        if (halted_) [[unlikely]] {
            if (tif != TimeInForce::GTC) {
                emit_cancel(id);
                return id;
            }
            rest(id, side, price, total_qty, owner, stp, display);
            return id;
        }
        if (tif == TimeInForce::FOK &&
            fillable(side, price, total_qty) < total_qty) {
            emit_cancel(id);
            return id;
        }
        if (tif == TimeInForce::PostOnly && would_cross(side, price)) {
            emit_cancel(id);
            return id;
        }
        place_limit(id, side, price, total_qty, tif, owner, stp, display);
        return id;
    }

    // Shared cancel body; also called from do_modify() so the re-entrancy
    // guard / drain is owned by exactly one Session per public call.
    bool cancel_impl(OrderId id) {
        Order* o = lookup(id);
        if (!o) return false;
        if (o->flags & kStopPending) [[unlikely]]
            remove_stop(o);
        else
            remove_resting(o);
        orders_[id] = nullptr;
        release(o);
        emit_cancel(id);
        return true;
    }

    bool do_reduce(OrderId id, Qty delta) {
        Order* o = lookup(id);
        if (!o) return false;
        if (o->flags & kStopPending) return false;  // cancel/resubmit instead
        if (o->flags & kIceberg) [[unlikely]] {
            // Shave the hidden reserve first (the displayed clip keeps its
            // size and priority), then the displayed remainder.
            Extra& x = extras_[static_cast<size_t>(o->ext)];
            if (delta >= o->qty + x.reserve) {
                remove_resting(o);
                orders_[id] = nullptr;
                release(o);
                return true;
            }
            Level& lvl = level_of(o);
            Qty from_reserve = (delta < x.reserve) ? delta : x.reserve;
            x.reserve  -= from_reserve;
            lvl.hidden -= from_reserve;
            Qty rest_delta = delta - from_reserve;
            lvl.total -= rest_delta;
            o->qty    -= rest_delta;
            return true;
        }
        if (delta >= o->qty) {
            remove_resting(o);
            orders_[id] = nullptr;
            release(o);
            return true;
        }
        level_of(o).total -= delta;
        o->qty -= delta;
        return true;
    }

    bool do_modify(OrderId id, Price new_price, Qty new_qty) {
        Order* o = lookup(id);
        if (!o) return false;
        if ((o->flags & kStopPending) && new_qty != 0)
            return false;                           // cancel/resubmit instead
        if (new_qty == 0) return cancel_impl(id);
        if (!in_band(new_price)) return false;

        // For icebergs `new_qty` means the new *total* (displayed +
        // hidden), mirroring how the order was submitted.
        Qty cur_total = o->qty;
        Qty peak = 0;
        if (o->flags & kIceberg) [[unlikely]] {
            const Extra& x = extras_[static_cast<size_t>(o->ext)];
            cur_total += x.reserve;
            peak = x.peak;
        }

        if (new_price == o->price && new_qty <= cur_total) {
            // In-place amend down: keeps time priority. Icebergs shave
            // reserve first, same as reduce().
            return do_reduce(id, cur_total - new_qty);
        }
        // Cancel-replace: loses priority, may match on re-entry. Emit the
        // cancel/accept pair so the event stream reflects the semantics --
        // the old resting order is gone and a new one (same id) is accepted,
        // mirroring submit_limit, which fires on_accept before it matches.
        // Owner, STP policy, and iceberg peak carry over to the re-entry.
        Side side = o->side;
        OwnerId owner = o->owner;
        StpPolicy stp = static_cast<StpPolicy>(o->flags & kStpMask);
        remove_resting(o);
        orders_[id] = nullptr;
        release(o);
        emit_cancel(id);
        emit_accept(id, side, new_price, new_qty);
        place_limit(id, side, new_price, new_qty, TimeInForce::GTC, owner, stp,
                    peak);
        return true;
    }

    // --- auction uncross ---------------------------------------------------

    // Consume `f` from the head order of `lvl` (displayed first, then any
    // iceberg reserve), removing it if fully spent. A part-consumed
    // iceberg is renormalized to a fresh clip so the post-auction book is
    // shaped exactly as if the clip had replenished. Returns true if the
    // level emptied.
    bool auction_fill(Level& lvl, Order* o, Qty f) {
        Qty from_disp = (f < o->qty) ? f : o->qty;
        o->qty    -= from_disp;
        lvl.total -= from_disp;
        Qty left = f - from_disp;
        if (o->flags & kIceberg) {
            Extra& x = extras_[static_cast<size_t>(o->ext)];
            x.reserve  -= left;   // left <= reserve by construction
            lvl.hidden -= left;
            if (o->qty == 0 && x.reserve > 0) {
                Qty clip = (x.peak < x.reserve) ? x.peak : x.reserve;
                x.reserve  -= clip;
                o->qty      = clip;
                lvl.total  += clip;
                lvl.hidden -= clip;
            }
        }
        if (o->qty == 0) {
            orders_[o->id] = nullptr;
            release(o);
            --lvl.count;
            lvl.head = o->next;   // o is always the head here
            if (lvl.head) lvl.head->prev = nullptr;
            else          lvl.tail = nullptr;
        }
        return lvl.head == nullptr;
    }

    Qty do_uncross() {
        if (best_bid_ < 0 || best_ask_ < 0 || best_bid_ < best_ask_)
            return 0;   // nothing overlaps (always true while continuous)
        const int64_t lo = best_ask_, hi = best_bid_;
        const size_t n = static_cast<size_t>(hi - lo + 1);

        // Cumulative executable volume across the overlap: supply[k] =
        // ask qty priced <= lo+k, demand[k] = bid qty priced >= lo+k.
        // Hidden reserve participates in full.
        std::vector<Qty> supply(n, 0), demand(n, 0);
        Qty acc = 0;
        for (size_t k = 0; k < n; ++k) {
            const Level& lvl = asks_[static_cast<size_t>(lo) + k];
            acc += lvl.total + lvl.hidden;
            supply[k] = acc;
        }
        acc = 0;
        for (size_t k = n; k-- > 0;) {
            const Level& lvl = bids_[static_cast<size_t>(lo) + k];
            acc += lvl.total + lvl.hidden;
            demand[k] = acc;
        }

        // Pick the clearing price: max executed volume, then least
        // leftover imbalance, then nearest the last trade, then lowest.
        size_t best_k = 0;
        Qty best_exec = 0, best_imb = 0;
        for (size_t k = 0; k < n; ++k) {
            Qty ex  = (demand[k] < supply[k]) ? demand[k] : supply[k];
            Qty imb = ((demand[k] > supply[k]) ? demand[k] - supply[k]
                                               : supply[k] - demand[k]);
            bool better = ex > best_exec;
            if (!better && ex == best_exec && ex > 0) {
                if (imb != best_imb) {
                    better = imb < best_imb;
                } else if (has_last_) {
                    int64_t da = min_ + lo + static_cast<int64_t>(k) - last_px_;
                    int64_t db = min_ + lo + static_cast<int64_t>(best_k) - last_px_;
                    if (da < 0) da = -da;
                    if (db < 0) db = -db;
                    better = da < db;
                }
            }
            if (better) { best_k = k; best_exec = ex; best_imb = imb; }
        }
        if (best_exec == 0) return 0;
        const int64_t p_idx = lo + static_cast<int64_t>(best_k);
        const Price p = min_ + p_idx;

        // Cross at p: price-time priority both sides, buy prints as taker.
        Qty crossed = 0;
        while (best_bid_ >= p_idx && best_ask_ >= 0 && best_ask_ <= p_idx) {
            Level& bl = bids_[static_cast<size_t>(best_bid_)];
            Level& al = asks_[static_cast<size_t>(best_ask_)];
            Order* b = bl.head;
            Order* a = al.head;
            Qty bq = b->qty, aq = a->qty;
            if (b->flags & kIceberg)
                bq += extras_[static_cast<size_t>(b->ext)].reserve;
            if (a->flags & kIceberg)
                aq += extras_[static_cast<size_t>(a->ext)].reserve;
            Qty f = (bq < aq) ? bq : aq;
            Trade t{b->id, a->id, p, f, Side::Buy};
            if (auction_fill(bl, b, f)) {
                bid_map_.clear(static_cast<size_t>(best_bid_));
                best_bid_ = bid_map_.find_le(best_bid_ - 1);
            }
            if (auction_fill(al, a, f)) {
                ask_map_.clear(static_cast<size_t>(best_ask_));
                best_ask_ = ask_map_.find_ge(best_ask_ + 1);
            }
            last_px_  = p;
            has_last_ = true;
            emit_trade(t);
            crossed += f;
        }
        return crossed;
    }

    // --- internals -------------------------------------------------------

    static constexpr uint8_t kStpMask     = 0x3;     // Order::flags bits 0-1
    static constexpr uint8_t kIceberg     = 1 << 2;  // Order::flags bit 2
    static constexpr uint8_t kStopPending = 1 << 3;  // parked in a stop book
    static constexpr uint8_t kStopMarket  = 1 << 4;  // stop-market on trigger

    int32_t alloc_extra() {
        if (extra_free_.empty()) {
            extras_.emplace_back();
            return static_cast<int32_t>(extras_.size() - 1);
        }
        int32_t i = extra_free_.back();
        extra_free_.pop_back();
        return i;
    }

    // Return a node (and its cold side-state, if any) to the pools.
    void release(Order* o) noexcept {
        if (o->ext >= 0) {
            extras_[static_cast<size_t>(o->ext)] = Extra{};
            extra_free_.push_back(o->ext);
        }
        pool_.free(o);
    }

    bool in_band(Price p) const noexcept {
        return p >= min_ && p < min_ + static_cast<Price>(n_levels_);
    }
    size_t idx(Price p) const noexcept { return static_cast<size_t>(p - min_); }

    Order* lookup(OrderId id) noexcept {
        return (id < orders_.size()) ? orders_[id] : nullptr;
    }

    Level& level_of(Order* o) noexcept {
        return (o->side == Side::Buy ? bids_ : asks_)[idx(o->price)];
    }

    OrderId next_id() {
        orders_.push_back(nullptr);
        return static_cast<OrderId>(orders_.size() - 1);
    }

    // Match, then rest (GTC/PostOnly) or cancel (IOC/FOK) the remainder.
    // `id` must already be registered. FOK feasibility is checked by the
    // caller, so an FOK reaching here fills completely unless STP stops
    // it; a PostOnly reaching here cannot cross (caller kills it
    // otherwise), so it rests in full. An STP halt cancels the remainder
    // outright -- it must not rest, or the next opposite-side own order
    // would face it again. A nonzero `peak` rests the remainder as an
    // iceberg: min(peak, remainder) displayed, the rest hidden.
    void place_limit(OrderId id, Side side, Price price, Qty qty,
                     TimeInForce tif = TimeInForce::GTC, OwnerId owner = 0,
                     StpPolicy stp = StpPolicy::CancelTaker, Qty peak = 0) {
        if (halted_) [[unlikely]] {   // modify() reprice during a call phase
            rest(id, side, price, qty, owner, stp, peak);
            return;
        }
        int64_t limit_idx = static_cast<int64_t>(idx(price));
        Qty remaining = (side == Side::Buy)
            ? match_buy(id, qty, limit_idx, owner, stp)
            : match_sell(id, qty, limit_idx, owner, stp);
        if (stp_halt_) {
            stp_halt_ = false;
            if (remaining > 0) emit_cancel(id);
            return;
        }
        if (remaining > 0) {
            if (tif == TimeInForce::IOC || tif == TimeInForce::FOK)
                emit_cancel(id);
            else
                rest(id, side, price, remaining, owner, stp, peak);
        }
    }

    // Would a limit at `price` take liquidity right now?
    bool would_cross(Side side, Price price) const noexcept {
        return (side == Side::Buy)
            ? (best_ask_ >= 0 && min_ + best_ask_ <= price)
            : (best_bid_ >= 0 && min_ + best_bid_ >= price);
    }

    // --- stop orders -------------------------------------------------------

    static bool stop_triggered(Side side, Price stop, Price last) noexcept {
        return (side == Side::Buy) ? (last >= stop) : (last <= stop);
    }

    void ensure_stop_books() {
        if (stop_bids_.empty()) {
            stop_bids_.assign(n_levels_, Level{});
            stop_asks_.assign(n_levels_, Level{});
            stop_bid_map_ = LevelBitmap(n_levels_);
            stop_ask_map_ = LevelBitmap(n_levels_);
        }
    }

    // Park a pending stop in the stop book keyed by trigger price. The
    // node reuses the intrusive-list fields; `price` holds the trigger,
    // and a stop-limit keeps its post-trigger limit in the side table.
    void rest_stop(OrderId id, Side side, Price stop_price, Price limit_price,
                   Qty qty, bool is_market, OwnerId owner, StpPolicy stp) {
        ensure_stop_books();
        Order* o = pool_.alloc();
        o->id = id; o->price = stop_price; o->qty = qty; o->side = side;
        o->owner = owner;
        o->flags = static_cast<uint8_t>(static_cast<uint8_t>(stp) |
                                        kStopPending |
                                        (is_market ? kStopMarket : 0));
        o->ext = -1;
        if (!is_market) {
            o->ext = alloc_extra();
            extras_[static_cast<size_t>(o->ext)].limit = limit_price;
        }
        o->prev = nullptr; o->next = nullptr;

        size_t i = idx(stop_price);
        auto& book = (side == Side::Buy) ? stop_bids_ : stop_asks_;
        auto& map  = (side == Side::Buy) ? stop_bid_map_ : stop_ask_map_;
        Level& lvl = book[i];
        if (lvl.tail) {
            lvl.tail->next = o;
            o->prev = lvl.tail;
            lvl.tail = o;
        } else {
            lvl.head = lvl.tail = o;
            map.set(i);
        }
        lvl.total += qty;
        ++lvl.count;
        ++pending_stops_;
        orders_[id] = o;
    }

    // Unlink a pending stop from its stop book. Does not free the node.
    void remove_stop(Order* o) {
        size_t i = idx(o->price);
        auto& book = (o->side == Side::Buy) ? stop_bids_ : stop_asks_;
        auto& map  = (o->side == Side::Buy) ? stop_bid_map_ : stop_ask_map_;
        Level& lvl = book[i];
        if (o->prev) o->prev->next = o->next; else lvl.head = o->next;
        if (o->next) o->next->prev = o->prev; else lvl.tail = o->prev;
        lvl.total -= o->qty;
        --lvl.count;
        --pending_stops_;
        if (lvl.head == nullptr) map.clear(i);
    }

    // The next stop the current last-trade price arms, in deterministic
    // order: buy stops first, ascending trigger (the order price rose
    // through them), then sell stops descending; FIFO within a level.
    Order* next_triggered_stop() {
        if (!has_last_ || stop_bids_.empty()) return nullptr;
        int64_t i = stop_bid_map_.find_ge(0);
        if (i >= 0 && min_ + i <= last_px_)
            return stop_bids_[static_cast<size_t>(i)].head;
        i = stop_ask_map_.find_le(static_cast<int64_t>(n_levels_) - 1);
        if (i >= 0 && min_ + i >= last_px_)
            return stop_asks_[static_cast<size_t>(i)].head;
        return nullptr;
    }

    // Fire a stop: market stops sweep and discard any remainder (market
    // semantics); stop-limits enter as GTC limits and may rest.
    void execute_stop(OrderId id, Side side, Price limit_price, Qty qty,
                      bool is_market, OwnerId owner, StpPolicy stp) {
        if (is_market) {
            (void)((side == Side::Buy)
                ? match_buy(id, qty, static_cast<int64_t>(n_levels_) - 1,
                            owner, stp)
                : match_sell(id, qty, 0, owner, stp));
            stp_halt_ = false;  // remainder is discarded either way
        } else {
            place_limit(id, side, limit_price, qty, TimeInForce::GTC, owner,
                        stp);
        }
    }

    // Fire every stop the tape has armed, including stops armed by the
    // fills of earlier stops (cascades run to fixpoint, in trigger order).
    // Nothing fires while halted; resume() pumps whatever the auction
    // prints armed.
    void pump_stops() {
        if (halted_) return;
        Order* s;
        while ((s = next_triggered_stop()) != nullptr) {
            OrderId id = s->id;
            Side side = s->side;
            Qty qty = s->qty;
            OwnerId owner = s->owner;
            StpPolicy stp = static_cast<StpPolicy>(s->flags & kStpMask);
            bool is_market = (s->flags & kStopMarket) != 0;
            Price limit_price =
                is_market ? 0 : extras_[static_cast<size_t>(s->ext)].limit;
            remove_stop(s);
            orders_[id] = nullptr;
            release(s);
            execute_stop(id, side, limit_price, qty, is_market, owner, stp);
        }
    }

    // Executable qty on the opposite side priced at-or-better than `price`,
    // capped at `want`: the scan stops as soon as enough is found. Hidden
    // iceberg reserve is executable, so it counts (FOK sees the whole
    // book, not just the displayed clips).
    Qty fillable(Side side, Price price, Qty want) const noexcept {
        int64_t limit = static_cast<int64_t>(idx(price));
        Qty sum = 0;
        if (side == Side::Buy) {
            for (int64_t i = best_ask_; i >= 0 && i <= limit && sum < want;
                 i = ask_map_.find_ge(i + 1)) {
                const Level& lvl = asks_[static_cast<size_t>(i)];
                sum += lvl.total + lvl.hidden;
            }
        } else {
            for (int64_t i = best_bid_; i >= 0 && i >= limit && sum < want;
                 i = bid_map_.find_le(i - 1)) {
                const Level& lvl = bids_[static_cast<size_t>(i)];
                sum += lvl.total + lvl.hidden;
            }
        }
        return sum;
    }

    // Aggressive buy: lift asks from best_ask_ upward while <= limit_idx.
    Qty match_buy(OrderId taker, Qty qty, int64_t limit_idx, OwnerId owner,
                  StpPolicy stp) {
        while (qty > 0 && best_ask_ >= 0 && best_ask_ <= limit_idx) {
            qty = sweep_level(asks_[best_ask_], taker, qty,
                              min_ + best_ask_, Side::Buy, owner, stp);
            if (asks_[best_ask_].head == nullptr) {
                ask_map_.clear(static_cast<size_t>(best_ask_));
                best_ask_ = ask_map_.find_ge(best_ask_ + 1);
            }
            if (stp_halt_) break;
        }
        return qty;
    }

    // Aggressive sell: hit bids from best_bid_ downward while >= limit_idx.
    Qty match_sell(OrderId taker, Qty qty, int64_t limit_idx, OwnerId owner,
                   StpPolicy stp) {
        while (qty > 0 && best_bid_ >= 0 && best_bid_ >= limit_idx) {
            qty = sweep_level(bids_[best_bid_], taker, qty,
                              min_ + best_bid_, Side::Sell, owner, stp);
            if (bids_[best_bid_].head == nullptr) {
                bid_map_.clear(static_cast<size_t>(best_bid_));
                best_bid_ = bid_map_.find_le(best_bid_ - 1);
            }
            if (stp_halt_) break;
        }
        return qty;
    }

    // Unlink and free the level head (the order the sweep is standing on),
    // emitting on_cancel. Returns the new head.
    Order* cancel_head(Level& lvl, Order* o) {
        Order* next = o->next;
        lvl.total -= o->qty;
        if (o->flags & kIceberg)
            lvl.hidden -= extras_[static_cast<size_t>(o->ext)].reserve;
        --lvl.count;
        orders_[o->id] = nullptr;
        emit_cancel(o->id);
        release(o);
        lvl.head = next;
        if (next) next->prev = nullptr;
        else      lvl.tail = nullptr;
        return next;
    }

    // Fill resting orders at one level FIFO. Returns taker qty remaining.
    // Sets stp_halt_ (and stops) if self-trade prevention says the taker
    // may not continue. An iceberg whose displayed clip is exhausted
    // replenishes from reserve and moves to the back of the level, so the
    // taker meets everyone else at the level before the next clip.
    Qty sweep_level(Level& lvl, OrderId taker, Qty qty, Price px,
                    Side taker_side, OwnerId taker_owner, StpPolicy stp) {
        Order* o = lvl.head;
        while (o != nullptr && qty > 0) {
            if (taker_owner != 0 && o->owner == taker_owner) [[unlikely]] {
                if (stp != StpPolicy::CancelTaker) o = cancel_head(lvl, o);
                if (stp == StpPolicy::CancelMaker) continue;
                stp_halt_ = true;   // CancelTaker / CancelBoth
                break;
            }
            Qty fill = (qty < o->qty) ? qty : o->qty;
            o->qty    -= fill;
            lvl.total -= fill;
            qty       -= fill;
            last_px_  = px;
            has_last_ = true;
            emit_trade(Trade{taker, o->id, px, fill, taker_side});
            if (o->qty == 0) {
                if (o->flags & kIceberg) [[unlikely]] {
                    Extra& x = extras_[static_cast<size_t>(o->ext)];
                    if (x.reserve > 0) {
                        Qty clip = (x.peak < x.reserve) ? x.peak : x.reserve;
                        x.reserve  -= clip;
                        o->qty      = clip;
                        lvl.total  += clip;
                        lvl.hidden -= clip;
                        if (o->next) {  // requeue behind the others
                            lvl.head = o->next;
                            lvl.head->prev = nullptr;
                            o->prev = lvl.tail;
                            o->next = nullptr;
                            lvl.tail->next = o;
                            lvl.tail = o;
                            o = lvl.head;
                        }
                        // alone at the level: stays put, keep sweeping it
                        continue;
                    }
                }
                Order* next = o->next;
                orders_[o->id] = nullptr;
                release(o);
                --lvl.count;
                o = next;
                lvl.head = o;
                if (o) o->prev = nullptr;
                else   lvl.tail = nullptr;
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty, OwnerId owner,
              StpPolicy stp, Qty peak = 0) {
        Order* o = pool_.alloc();
        o->id = id; o->price = price; o->qty = qty; o->side = side;
        o->owner = owner; o->flags = static_cast<uint8_t>(stp);
        o->ext = -1;
        o->prev = nullptr; o->next = nullptr;

        size_t i = idx(price);
        Level& lvl = (side == Side::Buy ? bids_ : asks_)[i];
        if (peak != 0 && qty > peak) {
            // Iceberg shape: show one clip, hide the rest.
            o->flags |= kIceberg;
            o->ext = alloc_extra();
            Extra& x = extras_[static_cast<size_t>(o->ext)];
            x.reserve = qty - peak;
            x.peak = peak;
            o->qty = peak;
            lvl.hidden += x.reserve;
        }
        if (lvl.tail) {
            lvl.tail->next = o;
            o->prev = lvl.tail;
            lvl.tail = o;
        } else {
            lvl.head = lvl.tail = o;
            if (side == Side::Buy) {
                bid_map_.set(i);
                if (static_cast<int64_t>(i) > best_bid_)
                    best_bid_ = static_cast<int64_t>(i);
            } else {
                ask_map_.set(i);
                if (best_ask_ < 0 || static_cast<int64_t>(i) < best_ask_)
                    best_ask_ = static_cast<int64_t>(i);
            }
        }
        lvl.total += o->qty;   // displayed only; reserve went to lvl.hidden
        ++lvl.count;
        orders_[id] = o;
    }

    // Unlink a resting order from its level; fix best/bitmap if the level
    // empties. Does not free the node or touch the id map.
    void remove_resting(Order* o) {
        size_t i = idx(o->price);
        Level& lvl = (o->side == Side::Buy ? bids_ : asks_)[i];
        if (o->prev) o->prev->next = o->next; else lvl.head = o->next;
        if (o->next) o->next->prev = o->prev; else lvl.tail = o->prev;
        lvl.total -= o->qty;
        if (o->flags & kIceberg)
            lvl.hidden -= extras_[static_cast<size_t>(o->ext)].reserve;
        --lvl.count;
        if (lvl.head == nullptr) {
            if (o->side == Side::Buy) {
                bid_map_.clear(i);
                if (best_bid_ == static_cast<int64_t>(i))
                    best_bid_ = bid_map_.find_le(best_bid_ - 1);
            } else {
                ask_map_.clear(i);
                if (best_ask_ == static_cast<int64_t>(i))
                    best_ask_ = ask_map_.find_ge(best_ask_ + 1);
            }
        }
    }

    Price  min_;
    size_t n_levels_;
    std::vector<Level> bids_, asks_;
    LevelBitmap bid_map_, ask_map_;
    int64_t best_bid_ = -1;   // level index, -1 = empty side
    int64_t best_ask_ = -1;
    Pool<Order> pool_;
    std::vector<Order*> orders_;  // id -> node (nullptr if gone/never rested)
    std::vector<Extra> extras_;   // cold side-state (icebergs, stops)
    std::vector<int32_t> extra_free_;
    // Stop books: pending stops keyed by trigger price, one Level array +
    // occupancy bitmap per side, allocated lazily on the first stop so
    // engines that never use stops pay nothing.
    std::vector<Level> stop_bids_, stop_asks_;
    LevelBitmap stop_bid_map_{0}, stop_ask_map_{0};
    size_t pending_stops_ = 0;
    Price last_px_ = 0;           // last trade price (stop triggers key off it)
    bool has_last_ = false;
    bool pumping_ = false;        // a pump_stops() loop is already running
    bool halted_ = false;         // auction call phase: no continuous matching
    Handler& h_;
    std::vector<Event> queue_;    // deferred-mode event buffer (unused if !DeferEvents)
    int depth_ = 0;               // re-entrancy nesting depth (see Session)
    bool stp_halt_ = false;       // sweep hit a same-owner resting order
};

// Fully re-entrant variant: Handler callbacks run after each operation
// completes, against a consistent book, and may call back into the engine.
template <typename Handler = NullHandler>
using ReentrantMatchingEngine = MatchingEngine<Handler, true>;

}  // namespace matchbook
