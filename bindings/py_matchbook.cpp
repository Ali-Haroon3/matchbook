// pybind11 bindings for research/backtest workflows. Optional component:
// the core library stays zero-dependency; build with -DMATCHBOOK_PYTHON=ON.
//
// Exposes the matching engine (with an internal trade collector so Python
// sees fills as a drainable list), the Avellaneda-Stoikov quoter, and the
// tabular Q-learning quoter.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <vector>

#include "matchbook/matching_engine.hpp"
#include "matchbook/strategy/avellaneda_stoikov.hpp"
#include "matchbook/strategy/rl_quoter.hpp"

namespace py = pybind11;
using namespace matchbook;

namespace {

// Collects engine events; Python drains them between calls.
struct Collector {
    std::vector<Trade> trades;
    std::vector<OrderId> cancels;
    uint64_t rejects = 0;
    void on_accept(OrderId, Side, Price, Qty) {}
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_cancel(OrderId id) { cancels.push_back(id); }
    void on_reject(OrderId) { ++rejects; }
};

// Owns the handler alongside the engine (the engine keeps a reference).
class PyEngine {
public:
    PyEngine(Price min_price, Price max_price, size_t expected_orders)
        : engine_(min_price, max_price, handler_, expected_orders) {}

    OrderId submit_limit(Side side, Price price, Qty qty, TimeInForce tif,
                         OwnerId owner, StpPolicy stp) {
        return engine_.submit_limit(side, price, qty, tif, owner, stp);
    }
    Qty submit_market(Side side, Qty qty, OwnerId owner, StpPolicy stp) {
        return engine_.submit_market(side, qty, owner, stp);
    }
    OrderId submit_iceberg(Side side, Price price, Qty total_qty, Qty display,
                           TimeInForce tif, OwnerId owner, StpPolicy stp) {
        return engine_.submit_iceberg(side, price, total_qty, display, tif,
                                      owner, stp);
    }
    OrderId submit_stop(Side side, Price stop_price, Qty qty, OwnerId owner,
                        StpPolicy stp) {
        return engine_.submit_stop(side, stop_price, qty, owner, stp);
    }
    OrderId submit_stop_limit(Side side, Price stop_price, Price limit_price,
                              Qty qty, OwnerId owner, StpPolicy stp) {
        return engine_.submit_stop_limit(side, stop_price, limit_price, qty,
                                         owner, stp);
    }
    bool cancel(OrderId id) { return engine_.cancel(id); }
    bool modify(OrderId id, Price price, Qty qty) {
        return engine_.modify(id, price, qty);
    }
    bool reduce(OrderId id, Qty delta) { return engine_.reduce(id, delta); }

    void halt() { engine_.halt(); }
    void resume() { engine_.resume(); }
    bool is_halted() const { return engine_.is_halted(); }
    Qty uncross() { return engine_.uncross(); }

    bool has_bid() const { return engine_.has_bid(); }
    bool has_ask() const { return engine_.has_ask(); }
    Price best_bid() const { return engine_.best_bid(); }
    Price best_ask() const { return engine_.best_ask(); }
    Qty depth_at(Side side, Price price) const {
        return engine_.depth_at(side, price);
    }
    Qty hidden_at(Side side, Price price) const {
        return engine_.hidden_at(side, price);
    }
    uint32_t order_count_at(Side side, Price price) const {
        return engine_.order_count_at(side, price);
    }
    std::vector<LevelView> top_levels(Side side, size_t max_levels) const {
        return engine_.top_levels(side, max_levels);
    }
    bool has_last_trade() const { return engine_.has_last_trade(); }
    Price last_trade() const { return engine_.last_trade(); }
    size_t pending_stops() const { return engine_.pending_stops(); }
    Qty stop_depth_at(Side side, Price price) const {
        return engine_.stop_depth_at(side, price);
    }
    size_t open_orders() const { return engine_.open_orders(); }
    uint64_t rejects() const { return handler_.rejects; }

    // Fills since the last call, oldest first.
    std::vector<Trade> take_trades() {
        std::vector<Trade> out;
        out.swap(handler_.trades);
        return out;
    }

    // Cancels (kills, STP removals, explicit cancels) since the last call.
    std::vector<OrderId> take_cancels() {
        std::vector<OrderId> out;
        out.swap(handler_.cancels);
        return out;
    }

private:
    Collector handler_;
    MatchingEngine<Collector> engine_;
};

}  // namespace

PYBIND11_MODULE(matchbook, m) {
    m.doc() = "matchbook: limit order book / matching engine bindings";

    py::enum_<Side>(m, "Side")
        .value("Buy", Side::Buy)
        .value("Sell", Side::Sell);

    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("GTC", TimeInForce::GTC)
        .value("IOC", TimeInForce::IOC)
        .value("FOK", TimeInForce::FOK)
        .value("PostOnly", TimeInForce::PostOnly);

    py::enum_<StpPolicy>(m, "StpPolicy")
        .value("CancelTaker", StpPolicy::CancelTaker)
        .value("CancelMaker", StpPolicy::CancelMaker)
        .value("CancelBoth", StpPolicy::CancelBoth);

    py::class_<LevelView>(m, "LevelView")
        .def_readonly("price", &LevelView::price)
        .def_readonly("qty", &LevelView::qty)
        .def_readonly("orders", &LevelView::orders)
        .def("__repr__", [](const LevelView& v) {
            return "LevelView(price=" + std::to_string(v.price) +
                   ", qty=" + std::to_string(v.qty) +
                   ", orders=" + std::to_string(v.orders) + ")";
        });

    py::class_<Trade>(m, "Trade")
        .def_readonly("taker", &Trade::taker)
        .def_readonly("maker", &Trade::maker)
        .def_readonly("price", &Trade::price)
        .def_readonly("qty", &Trade::qty)
        .def_readonly("taker_side", &Trade::taker_side)
        .def("__repr__", [](const Trade& t) {
            return "Trade(taker=" + std::to_string(t.taker) +
                   ", maker=" + std::to_string(t.maker) +
                   ", price=" + std::to_string(t.price) +
                   ", qty=" + std::to_string(t.qty) + ")";
        });

    py::class_<PyEngine>(m, "Engine")
        .def(py::init<Price, Price, size_t>(), py::arg("min_price"),
             py::arg("max_price"), py::arg("expected_orders") = size_t{1} << 20)
        .def("submit_limit", &PyEngine::submit_limit, py::arg("side"),
             py::arg("price"), py::arg("qty"),
             py::arg("tif") = TimeInForce::GTC, py::arg("owner") = 0,
             py::arg("stp") = StpPolicy::CancelTaker)
        .def("submit_market", &PyEngine::submit_market, py::arg("side"),
             py::arg("qty"), py::arg("owner") = 0,
             py::arg("stp") = StpPolicy::CancelTaker)
        .def("submit_iceberg", &PyEngine::submit_iceberg, py::arg("side"),
             py::arg("price"), py::arg("total_qty"), py::arg("display"),
             py::arg("tif") = TimeInForce::GTC, py::arg("owner") = 0,
             py::arg("stp") = StpPolicy::CancelTaker,
             "Rest showing at most `display`; clips replenish from the "
             "hidden reserve at the back of the level.")
        .def("submit_stop", &PyEngine::submit_stop, py::arg("side"),
             py::arg("stop_price"), py::arg("qty"), py::arg("owner") = 0,
             py::arg("stp") = StpPolicy::CancelTaker,
             "Market order once the last trade crosses the trigger.")
        .def("submit_stop_limit", &PyEngine::submit_stop_limit,
             py::arg("side"), py::arg("stop_price"), py::arg("limit_price"),
             py::arg("qty"), py::arg("owner") = 0,
             py::arg("stp") = StpPolicy::CancelTaker,
             "GTC limit at limit_price once the trigger trades.")
        .def("cancel", &PyEngine::cancel, py::arg("order_id"))
        .def("modify", &PyEngine::modify, py::arg("order_id"),
             py::arg("price"), py::arg("qty"))
        .def("reduce", &PyEngine::reduce, py::arg("order_id"), py::arg("delta"))
        .def("halt", &PyEngine::halt,
             "Suspend matching: GTC flow accumulates (book may cross).")
        .def("resume", &PyEngine::resume,
             "Return to continuous trading; stops armed by auction prints fire.")
        .def("is_halted", &PyEngine::is_halted)
        .def("uncross", &PyEngine::uncross,
             "Cross the overlap at the equilibrium price; returns executed qty.")
        .def("has_bid", &PyEngine::has_bid)
        .def("has_ask", &PyEngine::has_ask)
        .def("best_bid", &PyEngine::best_bid)
        .def("best_ask", &PyEngine::best_ask)
        .def("depth_at", &PyEngine::depth_at, py::arg("side"), py::arg("price"))
        .def("hidden_at", &PyEngine::hidden_at, py::arg("side"),
             py::arg("price"))
        .def("order_count_at", &PyEngine::order_count_at, py::arg("side"),
             py::arg("price"))
        .def("top_levels", &PyEngine::top_levels, py::arg("side"),
             py::arg("max_levels"),
             "Best-first aggregated levels: [LevelView(price, qty, orders)].")
        .def("has_last_trade", &PyEngine::has_last_trade)
        .def("last_trade", &PyEngine::last_trade)
        .def("pending_stops", &PyEngine::pending_stops)
        .def("stop_depth_at", &PyEngine::stop_depth_at, py::arg("side"),
             py::arg("stop_price"))
        .def("open_orders", &PyEngine::open_orders)
        .def("rejects", &PyEngine::rejects)
        .def("take_trades", &PyEngine::take_trades,
             "Fills since the last call, oldest first.")
        .def("take_cancels", &PyEngine::take_cancels,
             "Cancelled order ids since the last call, oldest first.");

    py::class_<strategy::ASParams>(m, "ASParams")
        .def(py::init<>())
        .def_readwrite("gamma", &strategy::ASParams::gamma)
        .def_readwrite("sigma", &strategy::ASParams::sigma)
        .def_readwrite("k", &strategy::ASParams::k)
        .def_readwrite("T", &strategy::ASParams::T);

    py::class_<strategy::QuotePair>(m, "QuotePair")
        .def_readonly("bid", &strategy::QuotePair::bid)
        .def_readonly("ask", &strategy::QuotePair::ask);

    py::class_<strategy::AvellanedaStoikov>(m, "AvellanedaStoikov")
        .def(py::init<strategy::ASParams>(), py::arg("params"))
        .def("quotes", &strategy::AvellanedaStoikov::quotes, py::arg("mid"),
             py::arg("inventory"), py::arg("t"))
        .def("reservation_price",
             &strategy::AvellanedaStoikov::reservation_price, py::arg("mid"),
             py::arg("inventory"), py::arg("t"));

    py::class_<strategy::RLParams>(m, "RLParams")
        .def(py::init<>())
        .def_readwrite("alpha", &strategy::RLParams::alpha)
        .def_readwrite("discount", &strategy::RLParams::discount)
        .def_readwrite("inv_bucket", &strategy::RLParams::inv_bucket)
        .def_readwrite("seed", &strategy::RLParams::seed);

    py::class_<strategy::RLQuoter>(m, "RLQuoter")
        .def(py::init<strategy::RLParams>(),
             py::arg("params") = strategy::RLParams{})
        .def("quotes", &strategy::RLQuoter::quotes, py::arg("mid"),
             py::arg("inventory"), py::arg("t") = 0.0)
        .def("learn", &strategy::RLQuoter::learn, py::arg("reward"),
             py::arg("new_inventory"))
        .def("set_epsilon", &strategy::RLQuoter::set_epsilon, py::arg("eps"))
        .def("bucket", &strategy::RLQuoter::bucket, py::arg("inventory"))
        .def("q_value", &strategy::RLQuoter::q_value, py::arg("state"),
             py::arg("action"));
}
