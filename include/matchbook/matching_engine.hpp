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
//     remainder, IOC cancels it, FOK executes fully or not at all.
//   * submit_market: matches until filled or the book is exhausted;
//     the remainder is discarded (never rests).
//   * cancel: O(1) removal by id.
//   * modify: qty decrease at the same price keeps time priority
//     (in-place amend); any price change or qty increase is treated as
//     cancel-replace and goes to the back of the queue, re-matching on
//     entry. This mirrors real exchange semantics (e.g. Nasdaq).
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

struct Order {
    OrderId id;
    Price   price;
    Qty     qty;      // remaining
    Side    side;
    Order*  prev;
    Order*  next;
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
    // on_cancel with no trades).
    OrderId submit_limit(Side side, Price price, Qty qty,
                         TimeInForce tif = TimeInForce::GTC) {
        return run([&] { return do_submit_limit(side, price, qty, tif); });
    }

    // Returns unfilled quantity (0 if fully filled).
    Qty submit_market(Side side, Qty qty) {
        return run([&] { return do_submit_market(side, qty); });
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

    // --- market data -----------------------------------------------------

    bool has_bid() const noexcept { return best_bid_ >= 0; }
    bool has_ask() const noexcept { return best_ask_ >= 0; }
    Price best_bid() const noexcept { return min_ + best_bid_; }
    Price best_ask() const noexcept { return min_ + best_ask_; }

    Qty depth_at(Side side, Price price) const noexcept {
        if (!in_band(price)) return 0;
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        return levels[idx(price)].total;
    }

    size_t open_orders() const noexcept { return pool_.in_use(); }

private:
    struct Level {
        Qty    total = 0;
        Order* head  = nullptr;
        Order* tail  = nullptr;
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

    // Run one public operation under a Session, then (deferred mode only)
    // dispatch its events once the outermost call unwinds. In the default
    // mode this inlines to the operation body plus, in debug, the guard.
    template <typename F>
    auto run(F&& f) {
        Session s(*this);
        auto result = f();
        if constexpr (DeferEvents) {
            if (s.outermost()) drain();
        }
        return result;
    }

    // --- operations ------------------------------------------------------

    OrderId do_submit_limit(Side side, Price price, Qty qty, TimeInForce tif) {
        if (!in_band(price) || qty == 0) [[unlikely]] {
            emit_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id();
        emit_accept(id, side, price, qty);
        if (tif == TimeInForce::FOK && fillable(side, price, qty) < qty) {
            emit_cancel(id);
            return id;
        }
        place_limit(id, side, price, qty, tif);
        return id;
    }

    Qty do_submit_market(Side side, Qty qty) {
        OrderId id = next_id();
        emit_accept(id, side, 0, qty);
        return (side == Side::Buy)
            ? match_buy(id, qty, static_cast<int64_t>(n_levels_) - 1)
            : match_sell(id, qty, 0);
    }

    // Shared cancel body; also called from do_modify() so the re-entrancy
    // guard / drain is owned by exactly one Session per public call.
    bool cancel_impl(OrderId id) {
        Order* o = lookup(id);
        if (!o) return false;
        remove_resting(o);
        orders_[id] = nullptr;
        pool_.free(o);
        emit_cancel(id);
        return true;
    }

    bool do_reduce(OrderId id, Qty delta) {
        Order* o = lookup(id);
        if (!o) return false;
        if (delta >= o->qty) {
            remove_resting(o);
            orders_[id] = nullptr;
            pool_.free(o);
            return true;
        }
        level_of(o).total -= delta;
        o->qty -= delta;
        return true;
    }

    bool do_modify(OrderId id, Price new_price, Qty new_qty) {
        Order* o = lookup(id);
        if (!o) return false;
        if (new_qty == 0) return cancel_impl(id);
        if (!in_band(new_price)) return false;

        if (new_price == o->price && new_qty <= o->qty) {
            // In-place amend down: keeps time priority.
            Qty delta = o->qty - new_qty;
            level_of(o).total -= delta;
            o->qty = new_qty;
            return true;
        }
        // Cancel-replace: loses priority, may match on re-entry. Emit the
        // cancel/accept pair so the event stream reflects the semantics --
        // the old resting order is gone and a new one (same id) is accepted,
        // mirroring submit_limit, which fires on_accept before it matches.
        Side side = o->side;
        remove_resting(o);
        orders_[id] = nullptr;
        pool_.free(o);
        emit_cancel(id);
        emit_accept(id, side, new_price, new_qty);
        place_limit(id, side, new_price, new_qty);
        return true;
    }

    // --- internals -------------------------------------------------------

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

    // Match, then rest (GTC) or cancel (IOC/FOK) the remainder. `id` must
    // already be registered. FOK feasibility is checked by the caller, so
    // an FOK reaching here always fills completely.
    void place_limit(OrderId id, Side side, Price price, Qty qty,
                     TimeInForce tif = TimeInForce::GTC) {
        int64_t limit_idx = static_cast<int64_t>(idx(price));
        Qty remaining = (side == Side::Buy)
            ? match_buy(id, qty, limit_idx)
            : match_sell(id, qty, limit_idx);
        if (remaining > 0) {
            if (tif == TimeInForce::GTC) rest(id, side, price, remaining);
            else                         emit_cancel(id);
        }
    }

    // Resting qty on the opposite side priced at-or-better than `price`,
    // capped at `want`: the scan stops as soon as enough is found.
    Qty fillable(Side side, Price price, Qty want) const noexcept {
        int64_t limit = static_cast<int64_t>(idx(price));
        Qty sum = 0;
        if (side == Side::Buy) {
            for (int64_t i = best_ask_; i >= 0 && i <= limit && sum < want;
                 i = ask_map_.find_ge(i + 1))
                sum += asks_[static_cast<size_t>(i)].total;
        } else {
            for (int64_t i = best_bid_; i >= 0 && i >= limit && sum < want;
                 i = bid_map_.find_le(i - 1))
                sum += bids_[static_cast<size_t>(i)].total;
        }
        return sum;
    }

    // Aggressive buy: lift asks from best_ask_ upward while <= limit_idx.
    Qty match_buy(OrderId taker, Qty qty, int64_t limit_idx) {
        while (qty > 0 && best_ask_ >= 0 && best_ask_ <= limit_idx) {
            qty = sweep_level(asks_[best_ask_], taker, qty,
                              min_ + best_ask_, Side::Buy);
            if (asks_[best_ask_].head == nullptr) {
                ask_map_.clear(static_cast<size_t>(best_ask_));
                best_ask_ = ask_map_.find_ge(best_ask_ + 1);
            }
        }
        return qty;
    }

    // Aggressive sell: hit bids from best_bid_ downward while >= limit_idx.
    Qty match_sell(OrderId taker, Qty qty, int64_t limit_idx) {
        while (qty > 0 && best_bid_ >= 0 && best_bid_ >= limit_idx) {
            qty = sweep_level(bids_[best_bid_], taker, qty,
                              min_ + best_bid_, Side::Sell);
            if (bids_[best_bid_].head == nullptr) {
                bid_map_.clear(static_cast<size_t>(best_bid_));
                best_bid_ = bid_map_.find_le(best_bid_ - 1);
            }
        }
        return qty;
    }

    // Fill resting orders at one level FIFO. Returns taker qty remaining.
    Qty sweep_level(Level& lvl, OrderId taker, Qty qty, Price px,
                    Side taker_side) {
        Order* o = lvl.head;
        while (o != nullptr && qty > 0) {
            Qty fill = (qty < o->qty) ? qty : o->qty;
            o->qty    -= fill;
            lvl.total -= fill;
            qty       -= fill;
            emit_trade(Trade{taker, o->id, px, fill, taker_side});
            if (o->qty == 0) {
                Order* next = o->next;
                orders_[o->id] = nullptr;
                pool_.free(o);
                o = next;
                lvl.head = o;
                if (o) o->prev = nullptr;
                else   lvl.tail = nullptr;
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Order* o = pool_.alloc();
        o->id = id; o->price = price; o->qty = qty; o->side = side;
        o->prev = nullptr; o->next = nullptr;

        size_t i = idx(price);
        Level& lvl = (side == Side::Buy ? bids_ : asks_)[i];
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
        lvl.total += qty;
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
    Handler& h_;
    std::vector<Event> queue_;    // deferred-mode event buffer (unused if !DeferEvents)
    int depth_ = 0;               // re-entrancy nesting depth (see Session)
};

// Fully re-entrant variant: Handler callbacks run after each operation
// completes, against a consistent book, and may call back into the engine.
template <typename Handler = NullHandler>
using ReentrantMatchingEngine = MatchingEngine<Handler, true>;

}  // namespace matchbook
