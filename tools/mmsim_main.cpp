// Market-making simulation: an Avellaneda-Stoikov agent quotes into the
// book while random background flow (passive orders + aggressive takers)
// trades around a mean-reverting fundamental. Compares AS against a naive
// symmetric quoter at the same spread budget to show what the inventory
// skew buys you.
//
//   usage: mmsim [steps] [gamma]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "matchbook/matching_engine.hpp"
#include "matchbook/strategy/avellaneda_stoikov.hpp"

using namespace matchbook;
using strategy::ASParams;
using strategy::AvellanedaStoikov;

namespace {

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint64_t next(uint64_t n) { return next() % n; }
    double uniform() { return static_cast<double>(next()) / 1.8446744073709552e19; }
};

// Tracks which fills belong to the market maker and accumulates PnL.
struct MMHandler {
    OrderId mm_bid = 0, mm_ask = 0;
    long    inventory = 0;
    double  cash = 0.0;
    uint64_t mm_fills = 0;

    void on_accept(OrderId, Side, Price, Qty) {}
    void on_cancel(OrderId) {}
    void on_reject(OrderId) {}
    void on_trade(const Trade& t) {
        // Our quotes only ever rest, so we're always the maker.
        if (t.maker == mm_bid) {
            inventory += static_cast<long>(t.qty);
            cash -= static_cast<double>(t.price) * t.qty;
            ++mm_fills;
        } else if (t.maker == mm_ask) {
            inventory -= static_cast<long>(t.qty);
            cash += static_cast<double>(t.price) * t.qty;
            ++mm_fills;
        }
    }
};

struct SimResult {
    double pnl;
    long   final_inv;
    long   max_abs_inv;
    uint64_t fills;
};

// mode 0 = Avellaneda-Stoikov, mode 1 = symmetric quotes at fixed spread
SimResult run(int steps, double gamma, int mode, uint64_t seed) {
    MMHandler h;
    MatchingEngine<MMHandler> e(1, 40000, h, 1 << 20);
    XorShift rng(seed);

    ASParams params;
    params.gamma = gamma;
    params.sigma = 2.0;
    params.k = 1.5;
    params.T = 1.0;
    AvellanedaStoikov as(params);

    double fundamental = 20000.0;
    long max_abs_inv = 0;
    const Qty quote_size = 5;

    for (int step = 0; step < steps; ++step) {
        double t = static_cast<double>(step) / steps;
        fundamental += (20000.0 - fundamental) * 0.001 +
                       (rng.uniform() - 0.5) * 4.0;  // OU-ish walk

        // Background passive flow keeps the book two-sided.
        for (int i = 0; i < 3; ++i) {
            Side s = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            int64_t off = 2 + static_cast<int64_t>(rng.next(20));
            Price px = static_cast<Price>(
                fundamental + (s == Side::Buy ? -off : off));
            e.submit_limit(s, px, 1 + rng.next(10));
        }

        // Re-quote: cancel old MM orders, place new ones.
        if (h.mm_bid) { e.cancel(h.mm_bid); h.mm_bid = 0; }
        if (h.mm_ask) { e.cancel(h.mm_ask); h.mm_ask = 0; }

        double mid = fundamental;
        if (e.has_bid() && e.has_ask())
            mid = 0.5 * (e.best_bid() + e.best_ask());

        Price bid_px, ask_px;
        if (mode == 0) {
            auto q = as.quotes(mid, h.inventory, t);
            bid_px = q.bid; ask_px = q.ask;
        } else {
            // Same average spread as AS at q=0, but no inventory skew.
            auto q0 = as.quotes(mid, 0, t);
            bid_px = q0.bid; ask_px = q0.ask;
        }
        // Quotes must not cross the market (we are a passive maker here).
        if (e.has_ask() && bid_px >= e.best_ask()) bid_px = e.best_ask() - 1;
        if (e.has_bid() && ask_px <= e.best_bid()) ask_px = e.best_bid() + 1;

        h.mm_bid = e.submit_limit(Side::Buy, bid_px, quote_size);
        h.mm_ask = e.submit_limit(Side::Sell, ask_px, quote_size);

        // Aggressive takers arrive and cross the spread.
        int n_takers = static_cast<int>(rng.next(4));
        for (int i = 0; i < n_takers; ++i) {
            e.submit_market((rng.next(2) == 0) ? Side::Buy : Side::Sell,
                            1 + rng.next(8));
        }

        long a = std::labs(h.inventory);
        if (a > max_abs_inv) max_abs_inv = a;
    }

    // Mark to market at the final mid.
    double mid = fundamental;
    if (e.has_bid() && e.has_ask())
        mid = 0.5 * (e.best_bid() + e.best_ask());
    double pnl = h.cash + h.inventory * mid;
    return {pnl, h.inventory, max_abs_inv, h.mm_fills};
}

}  // namespace

int main(int argc, char** argv) {
    int steps = (argc > 1) ? std::atoi(argv[1]) : 100000;
    double gamma = (argc > 2) ? std::atof(argv[2]) : 0.1;
    constexpr int kRuns = 5;

    std::printf("Avellaneda-Stoikov vs symmetric quoting, %d steps x %d runs, gamma=%.2f\n\n",
                steps, kRuns, gamma);
    std::printf("%-12s %12s %10s %12s %8s\n",
                "strategy", "PnL(ticks)", "final inv", "max |inv|", "fills");

    for (int mode = 0; mode <= 1; ++mode) {
        double pnl_sum = 0; long maxinv = 0; uint64_t fills = 0;
        long final_inv_abs_sum = 0;
        for (int r = 0; r < kRuns; ++r) {
            auto res = run(steps, gamma, mode, 1000 + 7919ull * r);
            pnl_sum += res.pnl;
            final_inv_abs_sum += std::labs(res.final_inv);
            if (res.max_abs_inv > maxinv) maxinv = res.max_abs_inv;
            fills += res.fills;
        }
        std::printf("%-12s %12.0f %10ld %12ld %8llu\n",
                    mode == 0 ? "AS" : "symmetric",
                    pnl_sum / kRuns, final_inv_abs_sum / kRuns, maxinv,
                    (unsigned long long)(fills / kRuns));
    }
    std::printf("\n(AS should carry materially less inventory risk for similar or better PnL.)\n");
    return 0;
}
