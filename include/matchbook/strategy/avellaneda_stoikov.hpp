#pragma once
// Avellaneda-Stoikov (2008) market-making quotes.
//
// The model gives closed-form optimal quotes for a market maker with
// exponential utility, inventory risk aversion gamma, mid-price volatility
// sigma, and order-arrival decay k over horizon [0, T]:
//
//   reservation price:  r(s, q, t) = s - q * gamma * sigma^2 * (T - t)
//   optimal spread:     delta      = gamma * sigma^2 * (T - t)
//                                    + (2 / gamma) * ln(1 + gamma / k)
//
// Intuition: the reservation price skews quotes against your inventory
// (long -> quote lower to shed, short -> quote higher to cover), and the
// spread widens with risk aversion, volatility, and time remaining.
//
// Off the hot path by design -- this runs on the strategy tick, not inside
// the matching loop, so doubles are fine here.
#include <cmath>

#include "../types.hpp"

namespace matchbook::strategy {

struct ASParams {
    double gamma = 0.1;    // inventory risk aversion
    double sigma = 2.0;    // mid-price volatility (ticks / sqrt(step))
    double k     = 1.5;    // liquidity/arrival decay
    double T     = 1.0;    // horizon (normalized)
};

struct QuotePair {
    Price bid;
    Price ask;
};

class AvellanedaStoikov {
public:
    explicit AvellanedaStoikov(ASParams p) : p_(p) {}

    // mid in ticks, inventory signed, t in [0, T].
    QuotePair quotes(double mid, long inventory, double t) const {
        const double tau = (p_.T > t) ? (p_.T - t) : 0.0;
        const double r =
            mid - static_cast<double>(inventory) * p_.gamma * p_.sigma * p_.sigma * tau;
        const double half_spread =
            0.5 * (p_.gamma * p_.sigma * p_.sigma * tau +
                   (2.0 / p_.gamma) * std::log(1.0 + p_.gamma / p_.k));
        Price bid = static_cast<Price>(std::floor(r - half_spread));
        Price ask = static_cast<Price>(std::ceil(r + half_spread));
        if (ask <= bid) ask = bid + 1;  // never cross yourself
        return {bid, ask};
    }

    double reservation_price(double mid, long inventory, double t) const {
        const double tau = (p_.T > t) ? (p_.T - t) : 0.0;
        return mid - static_cast<double>(inventory) * p_.gamma * p_.sigma * p_.sigma * tau;
    }

private:
    ASParams p_;
};

}  // namespace matchbook::strategy
