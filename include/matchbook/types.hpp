#pragma once
// matchbook: core types shared across the engine.
#include <cstdint>

namespace matchbook {

using OrderId = uint64_t;  // engine-assigned, sequential from 1
using Price   = int64_t;   // integer ticks (no floating point on the hot path)
using Qty     = uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// GTC: match, rest the remainder (default).
// IOC: match, cancel the remainder instead of resting it.
// FOK: fill the entire quantity immediately or execute nothing at all.
// PostOnly: rest without ever taking liquidity; killed if it would cross.
enum class TimeInForce : uint8_t { GTC = 0, IOC = 1, FOK = 2, PostOnly = 3 };

inline Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Participant id for self-trade prevention. 0 means "no owner": such
// orders never trigger STP, so the feature is free unless opted into.
using OwnerId = uint16_t;

// What to do when an incoming order would trade against a resting order
// with the same (nonzero) owner. The policy travels with the incoming
// order; the maker's own policy is irrelevant (exchange convention).
//   CancelTaker: stop matching, cancel the incoming remainder.
//   CancelMaker: cancel the resting order, keep matching.
//   CancelBoth:  cancel the resting order and the incoming remainder.
enum class StpPolicy : uint8_t { CancelTaker = 0, CancelMaker = 1, CancelBoth = 2 };

// Emitted once per fill. `maker` is the resting order, `taker` the incoming one.
struct Trade {
    OrderId taker;
    OrderId maker;
    Price   price;   // execution price = maker's limit price
    Qty     qty;
    Side    taker_side;
};

// Default event handler: all no-ops. The engine is templated on a Handler
// policy so event dispatch compiles down to nothing when unused -- no
// virtual calls or std::function on the hot path.
//
// A handler may additionally define
//     void on_rest(OrderId, Side, Price, Qty displayed);
// to be told whenever quantity becomes *visible* in the book: an order
// (or remainder) resting after matching, and each iceberg clip as it is
// replenished. Detected at compile time; handlers without it pay nothing.
// Market-data publishers need this hook -- accept announces intent, but
// only rest changes the displayed book.
struct NullHandler {
    void on_accept(OrderId, Side, Price, Qty) noexcept {}
    void on_trade(const Trade&) noexcept {}
    void on_cancel(OrderId) noexcept {}
    void on_reject(OrderId) noexcept {}
};

template <typename H>
concept HasOnRest = requires(H& h) {
    h.on_rest(OrderId{}, Side::Buy, Price{}, Qty{});
};

}  // namespace matchbook
