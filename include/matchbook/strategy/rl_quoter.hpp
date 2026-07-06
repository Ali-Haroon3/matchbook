#pragma once
// Tabular Q-learning market maker.
//
// State  = current inventory, clamped into coarse buckets.
// Action = (bid offset, ask offset) below/above mid, chosen independently.
//          Asymmetry is the learned analogue of the AS reservation-price
//          skew -- long inventory wants a tight ask and a wide bid, i.e.
//          shed by selling *more often*, never by selling through mid at a
//          loss -- and the agent has to discover that from the reward.
// Reward = supplied by the simulation (mark-to-market change minus an
//          inventory penalty); the quoter itself is reward-agnostic.
//
// Q-learning update: Q(s,a) += alpha * (r + discount * max_a' Q(s',a') - Q(s,a))
//
// Deliberately tiny: 61 states x 16 actions fits in one Q-table you can
// print and read. Same tick-level interface as AvellanedaStoikov so the
// simulator treats them interchangeably.
// ponytail: tabular over inventory only; add spread/imbalance features (or a
// function approximator) if the policy plateaus below AS.
#include <array>
#include <cmath>
#include <cstdint>

#include "avellaneda_stoikov.hpp"  // QuotePair

namespace matchbook::strategy {

struct RLParams {
    double   alpha      = 0.1;   // learning rate
    double   discount   = 0.99;  // future reward discount
    long     inv_bucket = 50;    // inventory units per state bucket
    uint64_t seed       = 42;    // exploration rng
};

class RLQuoter {
public:
    static constexpr int kInvRange = 30;  // buckets -30..+30
    static constexpr int kStates   = 2 * kInvRange + 1;
    // Ticks off mid. 32 is past the background-flow band, i.e. "pull this
    // side": the escape hatch AS gets from its unbounded reservation skew.
    static constexpr std::array<int, 4> kOffsets = {1, 2, 4, 32};
    static constexpr int kActions =
        static_cast<int>(kOffsets.size() * kOffsets.size());

    explicit RLQuoter(RLParams p = {}) : p_(p), rng_(p.seed ? p.seed : 1) {
        q_.fill(0.0);
    }

    // During training, call learn() once per step *before* the next quotes()
    // so the update sees the state the previous action led to.
    QuotePair quotes(double mid, long inventory, double /*t*/) {
        last_state_  = bucket(inventory);
        last_action_ = pick(last_state_);
        const int bid_off = kOffsets[static_cast<size_t>(last_action_) / kOffsets.size()];
        const int ask_off = kOffsets[static_cast<size_t>(last_action_) % kOffsets.size()];
        // Offsets >= 1 on both sides: quotes can't cross by construction.
        Price bid = static_cast<Price>(std::floor(mid)) - bid_off;
        Price ask = static_cast<Price>(std::ceil(mid)) + ask_off;
        return {bid, ask};
    }

    void learn(double reward, long new_inventory) {
        if (last_action_ < 0) return;
        const int s2 = bucket(new_inventory);
        double best = q_at(s2, 0);
        for (int a = 1; a < kActions; ++a)
            if (q_at(s2, a) > best) best = q_at(s2, a);
        double& q = q_at(last_state_, last_action_);
        q += p_.alpha * (reward + p_.discount * best - q);
    }

    void set_epsilon(double e) { epsilon_ = e; }

    int bucket(long inventory) const {
        long b = inventory / p_.inv_bucket;
        if (b < -kInvRange) b = -kInvRange;
        if (b > kInvRange) b = kInvRange;
        return static_cast<int>(b) + kInvRange;
    }

    double q_value(int state, int action) const { return q_at(state, action); }

private:
    int pick(int state) {
        if (epsilon_ > 0.0 && uniform() < epsilon_)
            return static_cast<int>(next() % kActions);
        int best = 0;
        for (int a = 1; a < kActions; ++a)
            if (q_at(state, a) > q_at(state, best)) best = a;
        return best;
    }

    double&       q_at(int s, int a)       { return q_[static_cast<size_t>(s) * kActions + a]; }
    const double& q_at(int s, int a) const { return q_[static_cast<size_t>(s) * kActions + a]; }

    uint64_t next() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return rng_; }
    double uniform() { return static_cast<double>(next()) / 1.8446744073709552e19; }

    RLParams p_;
    std::array<double, static_cast<size_t>(kStates) * kActions> q_;
    uint64_t rng_;
    double epsilon_ = 0.0;
    int last_state_ = 0;
    int last_action_ = -1;
};

}  // namespace matchbook::strategy
