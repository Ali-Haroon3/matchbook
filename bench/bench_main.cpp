// Benchmark: throughput and per-operation latency percentiles under a
// realistic mixed workload (adds around a drifting mid, cancels of live
// orders, occasional aggressive crossers), plus a two-thread SPSC
// feed-handler -> matching-engine pipeline benchmark.
//
// Methodology notes:
//  * Latency is sampled per-op with steady_clock; clock overhead is
//    measured first and reported so you can subtract it mentally.
//  * The book is pre-warmed so measurements reflect steady state, not an
//    empty book.
//  * Percentiles come from the full sorted sample, not a histogram.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "matchbook/matching_engine.hpp"
#include "matchbook/spsc_ring.hpp"

using namespace matchbook;
using Clock = std::chrono::steady_clock;

namespace {

struct CountingHandler {
    uint64_t trades = 0;
    uint64_t traded_qty = 0;
    void on_accept(OrderId, Side, Price, Qty) {}
    void on_trade(const Trade& t) { ++trades; traded_qty += t.qty; }
    void on_cancel(OrderId) {}
    void on_reject(OrderId) {}
};

struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint64_t next(uint64_t n) { return next() % n; }
};

constexpr Price kMinPx = 1;
constexpr Price kMaxPx = 20000;
constexpr Price kMid0  = 10000;

enum class OpKind : uint8_t { Add, Cancel, Cross };

struct Op {
    OpKind kind;
    Side   side;
    Price  price;
    Qty    qty;
    uint32_t cancel_slot;  // index into live-order table
};

// Pre-generate the op stream so RNG cost is excluded from measurement.
std::vector<Op> make_workload(size_t n, uint64_t seed) {
    XorShift rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    double mid = kMid0;
    for (size_t i = 0; i < n; ++i) {
        mid += (static_cast<double>(rng.next(200)) - 99.5) * 0.02;  // drift
        if (mid < kMid0 - 2000) mid = kMid0 - 2000;
        if (mid > kMid0 + 2000) mid = kMid0 + 2000;
        uint64_t roll = rng.next(100);
        Op op{};
        if (roll < 50) {  // passive add near mid
            op.kind = OpKind::Add;
            op.side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            int64_t off = 1 + static_cast<int64_t>(rng.next(40));
            op.price = static_cast<Price>(
                mid + (op.side == Side::Buy ? -off : off));
            op.qty = 1 + rng.next(100);
        } else if (roll < 90) {  // cancel a live order
            op.kind = OpKind::Cancel;
            op.cancel_slot = static_cast<uint32_t>(rng.next(1 << 16));
        } else {  // aggressive: cross the spread
            op.kind = OpKind::Cross;
            op.side = (rng.next(2) == 0) ? Side::Buy : Side::Sell;
            int64_t through = static_cast<int64_t>(rng.next(15));
            op.price = static_cast<Price>(
                mid + (op.side == Side::Buy ? through : -through));
            op.qty = 1 + rng.next(200);
        }
        ops.push_back(op);
    }
    return ops;
}

template <typename Engine>
inline void apply(Engine& e, const Op& op, std::vector<OrderId>& live) {
    switch (op.kind) {
        case OpKind::Add:
        case OpKind::Cross: {
            OrderId id = e.submit_limit(op.side, op.price, op.qty);
            live[id & 0xFFFF] = id;  // ring of recent ids to cancel from
            break;
        }
        case OpKind::Cancel: {
            OrderId id = live[op.cancel_slot];
            if (id) {
                e.cancel(id);
                live[op.cancel_slot] = 0;
            }
            break;
        }
    }
}

double clock_overhead_ns() {
    constexpr int N = 1 << 20;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        auto t = Clock::now();
        (void)t;
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
}

void bench_throughput(size_t n_ops) {
    CountingHandler h;
    MatchingEngine<CountingHandler> e(kMinPx, kMaxPx, h, 1 << 21);
    std::vector<OrderId> live(1 << 16, 0);

    auto warm = make_workload(1'000'000, 42);
    for (const auto& op : warm) apply(e, op, live);

    auto ops = make_workload(n_ops, 1337);
    auto t0 = Clock::now();
    for (const auto& op : ops) apply(e, op, live);
    auto t1 = Clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("== throughput ==\n");
    std::printf("ops:            %zu\n", n_ops);
    std::printf("elapsed:        %.3f s\n", sec);
    std::printf("throughput:     %.2f M ops/s\n", n_ops / sec / 1e6);
    std::printf("trades:         %llu (%.1f%% of ops produced fills)\n",
                (unsigned long long)h.trades, 100.0 * h.trades / n_ops);
    std::printf("open orders:    %zu\n\n", e.open_orders());
}

void bench_latency(size_t n_ops) {
    CountingHandler h;
    MatchingEngine<CountingHandler> e(kMinPx, kMaxPx, h, 1 << 21);
    std::vector<OrderId> live(1 << 16, 0);

    auto warm = make_workload(1'000'000, 7);
    for (const auto& op : warm) apply(e, op, live);

    auto ops = make_workload(n_ops, 99);
    std::vector<double> lat(n_ops);
    for (size_t i = 0; i < n_ops; ++i) {
        auto t0 = Clock::now();
        apply(e, ops[i], live);
        auto t1 = Clock::now();
        lat[i] = std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<size_t>(p * (n_ops - 1))]; };
    std::printf("== per-op latency (includes ~clock overhead below) ==\n");
    std::printf("samples:        %zu\n", n_ops);
    std::printf("clock overhead: %.0f ns/sample\n", clock_overhead_ns());
    std::printf("p50:            %.0f ns\n", pct(0.50));
    std::printf("p90:            %.0f ns\n", pct(0.90));
    std::printf("p99:            %.0f ns\n", pct(0.99));
    std::printf("p99.9:          %.0f ns\n", pct(0.999));
    std::printf("max:            %.0f ns\n\n", lat.back());
}

// Producer parses/generates ops, consumer runs the engine: models the
// feed-handler -> matching-thread architecture end to end.
void bench_pipeline(size_t n_ops) {
    static SpscRing<Op, 1 << 14> ring;
    auto ops = make_workload(n_ops, 2024);
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (const auto& op : ops) {
            while (!ring.try_push(op)) { /* spin: backpressure */ }
        }
        done.store(true, std::memory_order_release);
    });

    CountingHandler h;
    MatchingEngine<CountingHandler> e(kMinPx, kMaxPx, h, 1 << 21);
    std::vector<OrderId> live(1 << 16, 0);

    auto t0 = Clock::now();
    size_t consumed = 0;
    Op op;
    while (consumed < n_ops) {
        if (ring.try_pop(op)) {
            apply(e, op, live);
            ++consumed;
        } else if (done.load(std::memory_order_acquire) &&
                   ring.size_approx() == 0 && consumed >= n_ops) {
            break;
        }
    }
    auto t1 = Clock::now();
    producer.join();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("== SPSC pipeline (feed thread -> matching thread) ==\n");
    std::printf("ops:            %zu\n", n_ops);
    std::printf("throughput:     %.2f M ops/s end-to-end\n\n",
                n_ops / sec / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
    size_t n = (argc > 1) ? static_cast<size_t>(std::atoll(argv[1]))
                          : 10'000'000;
    std::printf("matchbook benchmark, %zu ops per section\n\n", n);
    bench_throughput(n);
    bench_latency(std::min<size_t>(n, 2'000'000));
    bench_pipeline(n);
    return 0;
}
