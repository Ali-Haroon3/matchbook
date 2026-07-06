// Replays an ITCH 5.0 file through the matching engine using the intended
// production shape: one thread parses the feed and normalizes messages,
// the other owns the book. They communicate over the lock-free SPSC ring.
//
//   usage: replay <file.itch> [SYMBOL]
//
// Works on tools/itchgen output and on real Nasdaq TotalView sample files.
// ITCH prices are in 1/10000 dollars; the book here ticks in the same
// units to avoid any rounding.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "matchbook/itch_parser.hpp"
#include "matchbook/matching_engine.hpp"
#include "matchbook/spsc_ring.hpp"

using namespace matchbook;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats {
    uint64_t trades = 0;
    void on_accept(OrderId, Side, Price, Qty) {}
    void on_trade(const Trade&) { ++trades; }
    void on_cancel(OrderId) {}
    void on_reject(OrderId) {}
};

SpscRing<itch::Message, 1 << 16> g_ring;
std::atomic<bool> g_feed_done{false};

void feed_thread(const std::string& path, const std::string& symbol,
                 uint64_t* parsed_out, uint64_t* matched_out) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> buf(1 << 20);
    uint64_t parsed = 0, matched = 0;
    char padded[9];
    std::snprintf(padded, sizeof(padded), "%-8s", symbol.c_str());

    uint8_t hdr[2];
    std::vector<uint8_t> body;
    while (in.read(reinterpret_cast<char*>(hdr), 2)) {
        size_t len = (static_cast<size_t>(hdr[0]) << 8) | hdr[1];
        body.resize(len);
        if (!in.read(reinterpret_cast<char*>(body.data()),
                     static_cast<std::streamsize>(len))) break;
        ++parsed;
        itch::Message m;
        if (!itch::parse(body.data(), len, m)) continue;
        // Symbol filter applies to adds; lifecycle msgs are ref-keyed and
        // filtered on the consumer side via the ref map.
        if (m.type == itch::MsgType::Add &&
            std::memcmp(m.stock, padded, 8) != 0) continue;
        ++matched;
        while (!g_ring.try_push(m)) { /* backpressure */ }
    }
    *parsed_out = parsed;
    *matched_out = matched;
    g_feed_done.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.itch> [SYMBOL]\n", argv[0]);
        return 1;
    }
    std::string path = argv[1];
    std::string symbol = (argc > 2) ? argv[2] : "MBTEST";

    uint64_t parsed = 0, matched = 0;
    std::thread feeder(feed_thread, path, symbol, &parsed, &matched);

    Stats h;
    // Band: $0.0001 .. $2000.0000 in 1/10000-dollar ticks.
    MatchingEngine<Stats> e(1, 20'000'000, h, 1 << 21);
    // ITCH order reference -> engine order id.
    std::unordered_map<uint64_t, OrderId> refmap;
    refmap.reserve(1 << 20);

    uint64_t applied = 0;
    auto t0 = Clock::now();
    itch::Message m;
    while (true) {
        if (!g_ring.try_pop(m)) {
            if (g_feed_done.load(std::memory_order_acquire) &&
                g_ring.size_approx() == 0) break;
            continue;
        }
        switch (m.type) {
            case itch::MsgType::Add: {
                OrderId id = e.submit_limit(m.side, m.price, m.qty);
                if (id != kInvalidOrderId) refmap[m.ref] = id;
                break;
            }
            case itch::MsgType::Execute:
            case itch::MsgType::Cancel: {
                auto it = refmap.find(m.ref);
                if (it == refmap.end()) break;
                // Partial reduce keeps priority (modify amend-down path);
                // full size-out removes the order.
                // We don't know remaining qty from the feed alone, so ask
                // the book: reduce by min(m.qty, resting).
                // Simplest correct handling: cancel if reduce-to-zero.
                // ITCH guarantees exec/cancel qty <= remaining.
                // Reduce via modify path:
                // (engine keeps priority on amend-down at same price)
                // We track nothing else per ref.
                // If modify fails (order gone), drop the ref.
                if (!e.reduce(it->second, m.qty)) refmap.erase(it);
                break;
            }
            case itch::MsgType::Delete: {
                auto it = refmap.find(m.ref);
                if (it != refmap.end()) {
                    e.cancel(it->second);
                    refmap.erase(it);
                }
                break;
            }
            case itch::MsgType::Replace: {
                auto it = refmap.find(m.ref);
                if (it != refmap.end()) {
                    OrderId id = it->second;
                    refmap.erase(it);
                    if (e.modify(id, m.price, m.qty)) refmap[m.new_ref] = id;
                }
                break;
            }
            default: break;
        }
        ++applied;
    }
    auto t1 = Clock::now();
    feeder.join();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("parsed:      %llu messages\n", (unsigned long long)parsed);
    std::printf("for %s: %llu messages applied\n", symbol.c_str(),
                (unsigned long long)applied);
    std::printf("elapsed:     %.3f s (%.2f M msgs/s applied)\n", sec,
                applied / sec / 1e6);
    std::printf("trades:      %llu\n", (unsigned long long)h.trades);
    if (e.has_bid() && e.has_ask()) {
        std::printf("final book:  bid %.4f x ask %.4f, %zu open orders\n",
                    e.best_bid() / 10000.0, e.best_ask() / 10000.0,
                    e.open_orders());
    } else {
        std::printf("final book:  one or both sides empty, %zu open orders\n",
                    e.open_orders());
    }
    return 0;
}
