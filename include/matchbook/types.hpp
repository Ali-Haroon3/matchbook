#pragma once
// matchbook: core types shared across the engine.
#include <cstdint>

namespace matchbook {

using OrderId = uint64_t;  // engine-assigned, sequential from 1
using Price   = int64_t;   // integer ticks (no floating point on the hot path)
using Qty     = uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

inline Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

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
struct NullHandler {
    void on_accept(OrderId, Side, Price, Qty) noexcept {}
    void on_trade(const Trade&) noexcept {}
    void on_cancel(OrderId) noexcept {}
    void on_reject(OrderId) noexcept {}
};

}  // namespace matchbook
