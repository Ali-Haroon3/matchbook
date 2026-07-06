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

#include <cassert>
#include <cstddef>
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

template <typename Handler = NullHandler>
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
        if (!in_band(price) || qty == 0) [[unlikely]] {
            h_.on_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id();
        h_.on_accept(id, side, price, qty);
        if (tif == TimeInForce::FOK && fillable(side, price, qty) < qty) {
            h_.on_cancel(id);
            return id;
        }
        place_limit(id, side, price, qty, tif);
        return id;
    }

    // Returns unfilled quantity (0 if fully filled).
    Qty submit_market(Side side, Qty qty) {
        OrderId id = next_id();
        h_.on_accept(id, side, 0, qty);
        Qty remaining = (side == Side::Buy)
            ? match_buy(id, qty, static_cast<int64_t>(n_levels_) - 1)
            : match_sell(id, qty, 0);
        return remaining;
    }

    bool cancel(OrderId id) {
        Order* o = lookup(id);
        if (!o) return false;
        remove_resting(o);
        orders_[id] = nullptr;
        pool_.free(o);
        h_.on_cancel(id);
        return true;
    }

    // Reduce a resting order's quantity by `delta` (feed-driven partial
    // execution or partial cancel). Keeps time priority; removes the order
    // if it reduces to zero. Returns false if the order is unknown.
    bool reduce(OrderId id, Qty delta) {
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

    // See semantics note at top of file.
    bool modify(OrderId id, Price new_price, Qty new_qty) {
        Order* o = lookup(id);
        if (!o) return false;
        if (new_qty == 0) return cancel(id);
        if (!in_band(new_price)) return false;

        if (new_price == o->price && new_qty <= o->qty) {
            // In-place amend down: keeps time priority.
            Qty delta = o->qty - new_qty;
            level_of(o).total -= delta;
            o->qty = new_qty;
            return true;
        }
        // Cancel-replace: loses priority, may match on re-entry.
        Side side = o->side;
        remove_resting(o);
        orders_[id] = nullptr;
        pool_.free(o);
        place_limit(id, side, new_price, new_qty);
        return true;
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
            else                         h_.on_cancel(id);
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
            h_.on_trade(Trade{taker, o->id, px, fill, taker_side});
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
};

}  // namespace matchbook
