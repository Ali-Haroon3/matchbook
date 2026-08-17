#pragma once
// Outbound market data: wraps a MatchingEngine and publishes its life as
// an ITCH 5.0 message stream describing the *displayed* book -- the
// missing half of the loop (OUCH in, match, ITCH out). Everything the
// repo already has for consuming ITCH applies to what this emits: the
// parser parses it, BookBuilder rebuilds an identical displayed book
// from it (the round-trip test asserts exactly that), `replay` replays
// a file of it.
//
// Event mapping, and why it is faithful:
//   * rest / iceberg clip replenish  -> 'A' with a fresh reference.
//     Accept is intent, rest is the book change; only rest publishes.
//     A replenished clip gets a new ref (its displayed predecessor died
//     at zero via executions), and lands at the back of its level --
//     the engine re-queues, the rebuilt book re-adds: same queue order.
//   * continuous fill               -> 'E' against the resting (maker)
//     ref. The aggressor is invisible, as on the real feed; if its
//     remainder rests, that is the 'A' above.
//   * auction fill                  -> 'C' (executed with price) against
//     each participating displayed ref, since the clearing price may
//     differ from the order's own limit; one aggregate 'Q' cross trade
//     closes the uncross. Hidden (iceberg reserve) participation is
//     visible only in the Q volume -- it was never in the displayed book.
//   * cancel of displayed qty       -> 'D' (full) via engine events, or
//     'X' (partial) computed by the reduce/modify wrappers, which diff
//     displayed qty across the call because in-place amends are
//     deliberately silent in the engine event vocabulary.
//   * reprice/upsize modify         -> 'D' + maker-side 'E's + 'A',
//     falling out of the engine's cancel-replace events untouched.
//     (Delete+add is spec-valid ITCH for a replace and reconstructs the
//     identical queue position.)
//   * halt() / resume()             -> 'H' stock trading action, so a
//     consumer can mirror the call phase (BookBuilder does: a pre-open
//     book may legitimately stand crossed).
//   * kills, rejects, pending stops -> nothing. They never touch the
//     displayed book.
//
// Timestamps are a monotonic message sequence (the engine is clockless);
// match numbers increment per execution message.
#include <cstdint>
#include <cstring>
#include <vector>

#include "itch_encode.hpp"
#include "matching_engine.hpp"
#include "types.hpp"

namespace matchbook::itch {

class Publisher {
public:
    struct Sink;   // engine event handler, defined below

    Publisher(Price min_price, Price max_price, const char* stock,
              size_t expected_orders = 1 << 20)
        : engine_(min_price, max_price, sink_, expected_orders) {
        sink_.pub = this;
        std::memset(stock_, ' ', sizeof(stock_));
        size_t n = std::strlen(stock);
        std::memcpy(stock_, stock, n < 8 ? n : 8);
        refs_.push_back(Ref{});   // id 0 is invalid
    }

    // --- order entry (same signatures as the engine) ---------------------

    OrderId submit_limit(Side side, Price price, Qty qty,
                         TimeInForce tif = TimeInForce::GTC,
                         OwnerId owner = 0,
                         StpPolicy stp = StpPolicy::CancelTaker) {
        return engine_.submit_limit(side, price, qty, tif, owner, stp);
    }
    Qty submit_market(Side side, Qty qty, OwnerId owner = 0,
                      StpPolicy stp = StpPolicy::CancelTaker) {
        return engine_.submit_market(side, qty, owner, stp);
    }
    OrderId submit_iceberg(Side side, Price price, Qty total_qty, Qty display,
                           TimeInForce tif = TimeInForce::GTC,
                           OwnerId owner = 0,
                           StpPolicy stp = StpPolicy::CancelTaker) {
        return engine_.submit_iceberg(side, price, total_qty, display, tif,
                                      owner, stp);
    }
    OrderId submit_stop(Side side, Price stop_price, Qty qty,
                        OwnerId owner = 0,
                        StpPolicy stp = StpPolicy::CancelTaker) {
        return engine_.submit_stop(side, stop_price, qty, owner, stp);
    }
    OrderId submit_stop_limit(Side side, Price stop_price, Price limit_price,
                              Qty qty, OwnerId owner = 0,
                              StpPolicy stp = StpPolicy::CancelTaker) {
        return engine_.submit_stop_limit(side, stop_price, limit_price, qty,
                                         owner, stp);
    }

    bool cancel(OrderId id) { return engine_.cancel(id); }

    // In-place amends are silent in the engine's event vocabulary, so the
    // wrappers diff displayed quantity across the call: a shrink is an
    // 'X' partial cancel, a disappearance is a 'D'. If the engine emitted
    // book events (cancel-replace), those already told the whole story.
    bool modify(OrderId id, Price new_price, Qty new_qty) {
        Qty before = engine_.order_qty(id);
        saw_book_event_ = false;
        bool ok = engine_.modify(id, new_price, new_qty);
        if (ok && !saw_book_event_ && before > 0)
            publish_shrink(id, before);
        return ok;
    }
    bool reduce(OrderId id, Qty delta) {
        Qty before = engine_.order_qty(id);
        bool ok = engine_.reduce(id, delta);
        if (ok && before > 0)
            publish_shrink(id, before);
        return ok;
    }

    // --- auction ---------------------------------------------------------

    void halt() {
        emit_action('H');
        engine_.halt();
    }
    void resume() {
        emit_action('T');    // before the pump: post-resume fills follow it
        engine_.resume();
    }
    bool is_halted() const noexcept { return engine_.is_halted(); }

    Qty uncross() {
        in_cross_ = true;
        Qty total = engine_.uncross();
        in_cross_ = false;
        if (total > 0) {
            std::vector<uint8_t> b;
            encode_cross(b, seq_++, total, stock_,
                         static_cast<uint32_t>(engine_.last_trade()),
                         ++match_);
            out_.push_back(std::move(b));
        }
        return total;
    }

    const MatchingEngine<Sink>& engine() const noexcept { return engine_; }

    // Published messages since the last call, oldest first (bodies only,
    // no framing) -- the shape mold::encode packs.
    std::vector<std::vector<uint8_t>> take_messages() {
        std::vector<std::vector<uint8_t>> v;
        v.swap(out_);
        return v;
    }

    // Drain into standard ITCH file framing (2-byte BE length + body):
    // the format itchgen writes and replay reads.
    void append_stream(std::vector<uint8_t>& out) {
        for (const auto& m : out_) frame(out, m);
        out_.clear();
    }

    // Engine event handler; public only because the engine type names it.
    struct Sink {
        Publisher* pub = nullptr;
        void on_accept(OrderId, Side, Price, Qty) {}
        void on_reject(OrderId) {}
        void on_trade(const Trade& t) { pub->ev_trade(t); }
        void on_cancel(OrderId id) { pub->ev_cancel(id); }
        void on_rest(OrderId id, Side s, Price p, Qty displayed) {
            pub->ev_rest(id, s, p, displayed);
        }
    };

private:
    struct Ref {
        uint64_t ref = 0;       // current ITCH reference (0 = not displayed)
        Qty displayed = 0;
    };

    Ref* slot(OrderId id) {
        return (id < refs_.size() && refs_[id].ref != 0) ? &refs_[id]
                                                         : nullptr;
    }

    void ev_rest(OrderId id, Side s, Price p, Qty displayed) {
        saw_book_event_ = true;
        if (refs_.size() <= id) refs_.resize(id + 1);
        refs_[id] = Ref{next_ref_++, displayed};
        std::vector<uint8_t> b;
        encode_add(b, seq_++, refs_[id].ref, s == Side::Buy ? 'B' : 'S',
                   static_cast<uint32_t>(displayed), stock_,
                   static_cast<uint32_t>(p));
        out_.push_back(std::move(b));
    }

    void ev_trade(const Trade& t) {
        if (in_cross_) {
            // Both sides rest in an auction; each displayed part prints
            // as 'C' at the clearing price. Reserve consumption has no
            // per-order message -- the closing 'Q' carries total volume.
            exec_cross(t.taker, t.qty, t.price);
            exec_cross(t.maker, t.qty, t.price);
            return;
        }
        Ref* r = slot(t.maker);
        if (!r) return;   // defensive; makers are always displayed
        std::vector<uint8_t> b;
        encode_execute(b, seq_++, r->ref, static_cast<uint32_t>(t.qty),
                       ++match_);
        out_.push_back(std::move(b));
        r->displayed -= (t.qty < r->displayed) ? t.qty : r->displayed;
        if (r->displayed == 0) r->ref = 0;   // died at zero, implicit
    }

    void exec_cross(OrderId id, Qty fill, Price px) {
        Ref* r = slot(id);
        if (!r) return;
        Qty part = (fill < r->displayed) ? fill : r->displayed;
        if (part == 0) return;
        std::vector<uint8_t> b;
        encode_execute_px(b, seq_++, r->ref, static_cast<uint32_t>(part),
                          ++match_, static_cast<uint32_t>(px));
        out_.push_back(std::move(b));
        r->displayed -= part;
        if (r->displayed == 0) r->ref = 0;
    }

    void ev_cancel(OrderId id) {
        saw_book_event_ = true;
        Ref* r = slot(id);
        if (!r) return;   // killed before ever resting: nothing to retract
        std::vector<uint8_t> b;
        encode_delete(b, seq_++, r->ref);
        out_.push_back(std::move(b));
        r->ref = 0;
        r->displayed = 0;
    }

    // After a silent in-place amend or reduce: X for a shrink, D if gone.
    void publish_shrink(OrderId id, Qty before) {
        Ref* r = slot(id);
        if (!r) return;
        Qty after = engine_.order_qty(id);
        std::vector<uint8_t> b;
        if (after == 0) {
            encode_delete(b, seq_++, r->ref);
            r->ref = 0;
            r->displayed = 0;
        } else if (after < before) {
            encode_cancel(b, seq_++, r->ref,
                          static_cast<uint32_t>(before - after));
            r->displayed = after;
        } else {
            return;   // reserve-only shave: displayed book unchanged
        }
        out_.push_back(std::move(b));
    }

    void emit_action(char state) {
        std::vector<uint8_t> b;
        encode_action(b, seq_++, stock_, state);
        out_.push_back(std::move(b));
    }

    Sink sink_;
    MatchingEngine<Sink> engine_;
    char stock_[8];
    std::vector<Ref> refs_;              // engine id -> displayed state
    std::vector<std::vector<uint8_t>> out_;
    uint64_t next_ref_ = 1;
    uint64_t seq_ = 1;
    uint64_t match_ = 0;
    bool in_cross_ = false;
    bool saw_book_event_ = false;
};

}  // namespace matchbook::itch
