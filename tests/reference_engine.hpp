#pragma once
// A deliberately naive reference implementation of the engine's public
// contract, for differential testing. Everything the fast engine does
// with flat arrays, bitmaps, intrusive lists, and pooled nodes, this
// does with std::map, std::deque, and linear scans -- slow and obvious
// on purpose. The lockstep fuzz drives both with identical operations
// and asserts identical event tapes (accepts, trades, cancels, rejects,
// rests) and identical book state after every operation.
//
// If a future optimization breaks a semantic corner, this is the wall
// it hits. Test-side only; ships nothing.
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "matchbook/types.hpp"

namespace refmodel {

using namespace matchbook;

template <typename Handler>
class ReferenceEngine {
public:
    ReferenceEngine(Price min_price, Price max_price, Handler& h)
        : min_(min_price), max_(max_price), h_(h) {}

    // --- order entry -----------------------------------------------------

    OrderId submit_limit(Side side, Price price, Qty qty,
                         TimeInForce tif = TimeInForce::GTC,
                         OwnerId owner = 0,
                         StpPolicy stp = StpPolicy::CancelTaker) {
        if (!in_band(price) || qty == 0) {
            h_.on_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id_++;
        h_.on_accept(id, side, price, qty);
        if (halted_) {
            if (tif != TimeInForce::GTC) h_.on_cancel(id);
            else rest(id, side, price, qty, 0, 0, owner, stp);
            return id;
        }
        if (tif == TimeInForce::FOK && fillable(side, price, qty) < qty) {
            h_.on_cancel(id);
            return id;
        }
        if (tif == TimeInForce::PostOnly && would_cross(side, price)) {
            h_.on_cancel(id);
            return id;
        }
        place(id, side, price, qty, tif, owner, stp, 0);
        pump();
        return id;
    }

    Qty submit_market(Side side, Qty qty, OwnerId owner = 0,
                      StpPolicy stp = StpPolicy::CancelTaker) {
        if (halted_) {
            h_.on_reject(kInvalidOrderId);
            return qty;
        }
        OrderId id = next_id_++;
        h_.on_accept(id, side, 0, qty);
        bool halt_flag = false;
        Qty remaining = match(id, side, qty, true, 0, owner, stp, halt_flag);
        pump();
        return remaining;
    }

    OrderId submit_iceberg(Side side, Price price, Qty total_qty, Qty display,
                           TimeInForce tif = TimeInForce::GTC,
                           OwnerId owner = 0,
                           StpPolicy stp = StpPolicy::CancelTaker) {
        if (!in_band(price) || total_qty == 0 || display == 0) {
            h_.on_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id_++;
        h_.on_accept(id, side, price, total_qty);
        if (halted_) {
            if (tif != TimeInForce::GTC) h_.on_cancel(id);
            else rest_shaped(id, side, price, total_qty, display, owner, stp);
            return id;
        }
        if (tif == TimeInForce::FOK &&
            fillable(side, price, total_qty) < total_qty) {
            h_.on_cancel(id);
            return id;
        }
        if (tif == TimeInForce::PostOnly && would_cross(side, price)) {
            h_.on_cancel(id);
            return id;
        }
        place(id, side, price, total_qty, tif, owner, stp, display);
        pump();
        return id;
    }

    OrderId submit_stop(Side side, Price stop_price, Qty qty,
                        OwnerId owner = 0,
                        StpPolicy stp = StpPolicy::CancelTaker) {
        return do_stop(side, stop_price, 0, qty, true, owner, stp);
    }
    OrderId submit_stop_limit(Side side, Price stop_price, Price limit_price,
                              Qty qty, OwnerId owner = 0,
                              StpPolicy stp = StpPolicy::CancelTaker) {
        return do_stop(side, stop_price, limit_price, qty, false, owner, stp);
    }

    bool cancel(OrderId id) {
        for (auto* book : {&bids_, &asks_}) {
            for (auto& [px, level] : *book) {
                for (size_t i = 0; i < level.size(); ++i) {
                    if (level[i].id != id) continue;
                    level.erase(level.begin() +
                                static_cast<ptrdiff_t>(i));
                    if (level.empty()) book->erase(px);
                    h_.on_cancel(id);
                    pump();
                    return true;
                }
            }
        }
        for (size_t i = 0; i < stops_.size(); ++i) {
            if (stops_[i].id != id) continue;
            stops_.erase(stops_.begin() + static_cast<ptrdiff_t>(i));
            h_.on_cancel(id);
            pump();
            return true;
        }
        return false;
    }

    bool reduce(OrderId id, Qty delta) {
        RO* o = find_resting(id);
        if (!o) return false;
        if (delta >= o->qty + o->reserve) {
            erase_resting(id);          // silent, like the engine
        } else {
            Qty from_reserve = (delta < o->reserve) ? delta : o->reserve;
            o->reserve -= from_reserve;
            o->qty -= (delta - from_reserve);
        }
        pump();
        return true;
    }

    bool modify(OrderId id, Price new_price, Qty new_qty) {
        if (find_stop(id) && new_qty != 0) return false;
        if (new_qty == 0) return cancel(id);
        if (!in_band(new_price)) return false;
        RO* o = find_resting(id);
        if (!o) return false;

        Qty cur_total = o->qty + o->reserve;
        if (new_price == o->price && new_qty <= cur_total)
            return reduce(id, cur_total - new_qty);

        RO copy = *o;
        erase_resting(id);
        h_.on_cancel(id);
        h_.on_accept(id, copy.side, new_price, new_qty);
        place(id, copy.side, new_price, new_qty, TimeInForce::GTC, copy.owner,
              copy.stp, copy.peak);
        pump();
        return true;
    }

    // --- auction ---------------------------------------------------------

    void halt() { halted_ = true; }
    void resume() {
        halted_ = false;
        pump();
    }
    bool is_halted() const { return halted_; }

    Qty uncross() {
        Qty crossed = do_uncross();
        pump();
        return crossed;
    }

    // --- state (compared against the fast engine) ------------------------

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    Price best_bid() const { return bids_.rbegin()->first; }
    Price best_ask() const { return asks_.begin()->first; }
    bool has_last_trade() const { return has_last_; }
    Price last_trade() const { return last_; }
    size_t pending_stops() const { return stops_.size(); }

    Qty depth_at(Side s, Price px) const {
        const auto& book = (s == Side::Buy) ? bids_ : asks_;
        auto it = book.find(px);
        if (it == book.end()) return 0;
        Qty sum = 0;
        for (const auto& o : it->second) sum += o.qty;
        return sum;
    }
    Qty hidden_at(Side s, Price px) const {
        const auto& book = (s == Side::Buy) ? bids_ : asks_;
        auto it = book.find(px);
        if (it == book.end()) return 0;
        Qty sum = 0;
        for (const auto& o : it->second) sum += o.reserve;
        return sum;
    }
    uint32_t order_count_at(Side s, Price px) const {
        const auto& book = (s == Side::Buy) ? bids_ : asks_;
        auto it = book.find(px);
        return it == book.end() ? 0
                                : static_cast<uint32_t>(it->second.size());
    }
    size_t open_orders() const {
        size_t n = stops_.size();
        for (const auto* book : {&bids_, &asks_})
            for (const auto& [px, level] : *book) n += level.size();
        return n;
    }
    // Occupied levels best-first, as (price, displayed, count) triples.
    std::vector<std::array<long long, 3>> levels(Side s) const {
        std::vector<std::array<long long, 3>> v;
        auto add = [&](Price px, const std::deque<RO>& level) {
            long long q = 0;
            for (const auto& o : level) q += static_cast<long long>(o.qty);
            v.push_back({px, q, static_cast<long long>(level.size())});
        };
        if (s == Side::Buy)
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
                add(it->first, it->second);
        else
            for (const auto& [px, level] : asks_) add(px, level);
        return v;
    }

private:
    struct RO {
        OrderId id;
        Side side;
        Price price;         // resting: limit; pending stop: trigger
        Qty qty;             // displayed
        Qty reserve = 0;
        Qty peak = 0;
        OwnerId owner = 0;
        StpPolicy stp = StpPolicy::CancelTaker;
        bool stop_market = false;
        Price stop_limit = 0;
    };

    bool in_band(Price p) const { return p >= min_ && p <= max_; }

    bool would_cross(Side side, Price price) const {
        if (side == Side::Buy)
            return has_ask() && best_ask() <= price;
        return has_bid() && best_bid() >= price;
    }

    Qty fillable(Side side, Price price, Qty want) const {
        Qty sum = 0;
        if (side == Side::Buy) {
            for (const auto& [px, level] : asks_) {
                if (px > price || sum >= want) break;
                for (const auto& o : level) sum += o.qty + o.reserve;
            }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (it->first < price || sum >= want) break;
                for (const auto& o : it->second) sum += o.qty + o.reserve;
            }
        }
        return sum;
    }

    RO* find_resting(OrderId id) {
        for (auto* book : {&bids_, &asks_})
            for (auto& [px, level] : *book)
                for (auto& o : level)
                    if (o.id == id) return &o;
        return nullptr;
    }
    RO* find_stop(OrderId id) {
        for (auto& o : stops_)
            if (o.id == id) return &o;
        return nullptr;
    }
    void erase_resting(OrderId id) {
        for (auto* book : {&bids_, &asks_}) {
            for (auto& [px, level] : *book) {
                for (size_t i = 0; i < level.size(); ++i) {
                    if (level[i].id != id) continue;
                    level.erase(level.begin() +
                                static_cast<ptrdiff_t>(i));
                    if (level.empty()) book->erase(px);
                    return;
                }
            }
        }
    }

    void rest(OrderId id, Side side, Price price, Qty qty, Qty reserve,
              Qty peak, OwnerId owner, StpPolicy stp) {
        RO o{};
        o.id = id; o.side = side; o.price = price; o.qty = qty;
        o.reserve = reserve; o.peak = peak; o.owner = owner; o.stp = stp;
        auto& book = (side == Side::Buy) ? bids_ : asks_;
        book[price].push_back(o);
        h_.on_rest(id, side, price, qty);
    }

    // Rest with iceberg shaping (mirrors place_limit's peak handling).
    void rest_shaped(OrderId id, Side side, Price price, Qty qty, Qty peak,
                     OwnerId owner, StpPolicy stp) {
        if (peak != 0 && qty > peak)
            rest(id, side, price, peak, qty - peak, peak, owner, stp);
        else
            rest(id, side, price, qty, 0, 0, owner, stp);
    }

    // Match an aggressive order. Returns unfilled qty; sets halt_flag if
    // STP stopped the taker (CancelTaker/CancelBoth).
    Qty match(OrderId taker, Side side, Qty qty, bool is_market, Price limit,
              OwnerId owner, StpPolicy stp, bool& halt_flag) {
        auto& opp = (side == Side::Buy) ? asks_ : bids_;
        while (qty > 0 && !opp.empty()) {
            Price px = (side == Side::Buy) ? opp.begin()->first
                                           : opp.rbegin()->first;
            if (!is_market) {
                if (side == Side::Buy ? px > limit : px < limit) break;
            }
            auto& level = opp[px];
            while (!level.empty() && qty > 0) {
                RO& o = level.front();
                if (owner != 0 && o.owner == owner) {
                    if (stp != StpPolicy::CancelTaker) {
                        OrderId dead = o.id;
                        level.pop_front();
                        h_.on_cancel(dead);
                    }
                    if (stp == StpPolicy::CancelMaker) continue;
                    halt_flag = true;   // CancelTaker / CancelBoth
                    break;
                }
                Qty fill = (qty < o.qty) ? qty : o.qty;
                o.qty -= fill;
                qty -= fill;
                last_ = px;
                has_last_ = true;
                h_.on_trade(Trade{taker, o.id, px, fill, side});
                if (o.qty == 0) {
                    if (o.reserve > 0) {
                        Qty clip = (o.peak < o.reserve) ? o.peak : o.reserve;
                        o.reserve -= clip;
                        o.qty = clip;
                        h_.on_rest(o.id, o.side, px, clip);
                        RO moved = o;
                        level.pop_front();
                        level.push_back(moved);
                    } else {
                        level.pop_front();
                    }
                }
            }
            if (level.empty()) opp.erase(px);
            if (halt_flag) break;
        }
        return qty;
    }

    void place(OrderId id, Side side, Price price, Qty qty, TimeInForce tif,
               OwnerId owner, StpPolicy stp, Qty peak) {
        if (halted_) {
            rest_shaped(id, side, price, qty, peak, owner, stp);
            return;
        }
        bool halt_flag = false;
        Qty remaining =
            match(id, side, qty, false, price, owner, stp, halt_flag);
        if (halt_flag) {
            if (remaining > 0) h_.on_cancel(id);
            return;
        }
        if (remaining > 0) {
            if (tif == TimeInForce::IOC || tif == TimeInForce::FOK)
                h_.on_cancel(id);
            else
                rest_shaped(id, side, price, remaining, peak, owner, stp);
        }
    }

    OrderId do_stop(Side side, Price stop_price, Price limit_price, Qty qty,
                    bool is_market, OwnerId owner, StpPolicy stp) {
        if (!in_band(stop_price) || qty == 0 ||
            (!is_market && !in_band(limit_price))) {
            h_.on_reject(kInvalidOrderId);
            return kInvalidOrderId;
        }
        OrderId id = next_id_++;
        h_.on_accept(id, side, stop_price, qty);
        if (!halted_ && has_last_ && triggered(side, stop_price)) {
            fire(id, side, limit_price, qty, is_market, owner, stp);
            pump();
            return id;
        }
        RO o{};
        o.id = id; o.side = side; o.price = stop_price; o.qty = qty;
        o.owner = owner; o.stp = stp;
        o.stop_market = is_market; o.stop_limit = limit_price;
        stops_.push_back(o);
        pump();
        return id;
    }

    bool triggered(Side side, Price stop) const {
        return (side == Side::Buy) ? (last_ >= stop) : (last_ <= stop);
    }

    void fire(OrderId id, Side side, Price limit_price, Qty qty,
              bool is_market, OwnerId owner, StpPolicy stp) {
        if (is_market) {
            bool halt_flag = false;
            (void)match(id, side, qty, true, 0, owner, stp, halt_flag);
        } else {
            place(id, side, limit_price, qty, TimeInForce::GTC, owner, stp,
                  0);
        }
    }

    // Buy stops ascending trigger, then sell stops descending; FIFO among
    // equal triggers (stops_ is insertion-ordered).
    void pump() {
        if (halted_ || !has_last_) return;
        for (;;) {
            size_t pick = stops_.size();
            for (size_t i = 0; i < stops_.size(); ++i) {
                const RO& o = stops_[i];
                if (o.side != Side::Buy || !triggered(o.side, o.price))
                    continue;
                if (pick == stops_.size() ||
                    o.price < stops_[pick].price)
                    pick = i;
            }
            if (pick == stops_.size()) {
                for (size_t i = 0; i < stops_.size(); ++i) {
                    const RO& o = stops_[i];
                    if (o.side != Side::Sell || !triggered(o.side, o.price))
                        continue;
                    if (pick == stops_.size() ||
                        o.price > stops_[pick].price)
                        pick = i;
                }
            }
            if (pick == stops_.size()) return;
            RO o = stops_[pick];
            stops_.erase(stops_.begin() + static_cast<ptrdiff_t>(pick));
            fire(o.id, o.side, o.stop_limit, o.qty, o.stop_market, o.owner,
                 o.stp);
        }
    }

    Qty do_uncross() {
        if (bids_.empty() || asks_.empty() ||
            best_bid() < best_ask())
            return 0;
        const Price lo = best_ask(), hi = best_bid();

        auto executable = [](const std::deque<RO>& level) {
            Qty q = 0;
            for (const auto& o : level) q += o.qty + o.reserve;
            return q;
        };
        Qty best_exec = 0, best_imb = 0;
        Price clearing = lo;
        for (Price p = lo; p <= hi; ++p) {
            Qty demand = 0, supply = 0;
            for (const auto& [px, level] : bids_)
                if (px >= p) demand += executable(level);
            for (const auto& [px, level] : asks_)
                if (px <= p) supply += executable(level);
            Qty ex = (demand < supply) ? demand : supply;
            Qty imb = (demand > supply) ? demand - supply : supply - demand;
            bool better = ex > best_exec;
            if (!better && ex == best_exec && ex > 0) {
                if (imb != best_imb) {
                    better = imb < best_imb;
                } else if (has_last_) {
                    Price da = p > last_ ? p - last_ : last_ - p;
                    Price db = clearing > last_ ? clearing - last_
                                                : last_ - clearing;
                    better = da < db;
                }
            }
            if (better) { best_exec = ex; best_imb = imb; clearing = p; }
        }
        if (best_exec == 0) return 0;

        Qty crossed = 0;
        while (!bids_.empty() && !asks_.empty() && best_bid() >= clearing &&
               best_ask() <= clearing) {
            Price bpx = best_bid(), apx = best_ask();
            auto& bl = bids_[bpx];
            auto& al = asks_[apx];
            RO& b = bl.front();
            RO& a = al.front();
            Qty bq = b.qty + b.reserve, aq = a.qty + a.reserve;
            Qty f = (bq < aq) ? bq : aq;
            last_ = clearing;
            has_last_ = true;
            h_.on_trade(Trade{b.id, a.id, clearing, f, Side::Buy});
            cross_consume(bl, f, clearing);
            if (bl.empty()) bids_.erase(bpx);
            cross_consume(al, f, clearing);
            if (al.empty()) asks_.erase(apx);
            crossed += f;
        }
        return crossed;
    }

    // Consume `f` from the front order: displayed first, then reserve; a
    // part-consumed iceberg renormalizes and re-queues (like the engine).
    void cross_consume(std::deque<RO>& level, Qty f, Price px) {
        RO& o = level.front();
        Qty from_disp = (f < o.qty) ? f : o.qty;
        o.qty -= from_disp;
        Qty left = f - from_disp;
        o.reserve -= left;
        if (o.qty == 0 && o.reserve > 0) {
            Qty clip = (o.peak < o.reserve) ? o.peak : o.reserve;
            o.reserve -= clip;
            o.qty = clip;
            h_.on_rest(o.id, o.side, px, clip);
            RO moved = o;
            level.pop_front();
            level.push_back(moved);
            return;
        }
        if (o.qty == 0) level.pop_front();
    }

    Price min_, max_;
    Handler& h_;
    std::map<Price, std::deque<RO>> bids_, asks_;
    std::vector<RO> stops_;       // insertion order = FIFO within a trigger
    OrderId next_id_ = 1;
    Price last_ = 0;
    bool has_last_ = false;
    bool halted_ = false;
};

}  // namespace refmodel
