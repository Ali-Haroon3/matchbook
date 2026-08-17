// Replays an ITCH 5.0 file through the matching engine using the intended
// production shape: one thread parses the feed and normalizes messages,
// the other owns the books. They communicate over the lock-free SPSC ring.
//
//   usage: replay <file.itch> [SYMBOL...]
//
// One or more symbols build side by side in a Venue (default: MBTEST,
// itchgen's symbol). Each book is registered lazily on the symbol's
// first Add, banded +/- $13.11 (1<<17 ticks) around that price -- adds
// that later run outside the band are counted as dropped rather than
// growing the book without bound. Works on tools/itchgen output and on
// real Nasdaq TotalView sample files. ITCH prices are in 1/10000
// dollars; the books tick in the same units to avoid any rounding.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "matchbook/itch_book_builder.hpp"
#include "matchbook/itch_parser.hpp"
#include "matchbook/spsc_ring.hpp"
#include "matchbook/venue.hpp"

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

std::string pad8(const std::string& s) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%-8s", s.c_str());
    return std::string(buf, 8);
}

void feed_thread(const std::string& path,
                 const std::unordered_set<std::string>& symbols,
                 uint64_t* parsed_out, uint64_t* matched_out) {
    std::ifstream in(path, std::ios::binary);
    uint64_t parsed = 0, matched = 0;

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
        // Symbol filter applies to adds (and trading actions); lifecycle
        // msgs are ref-keyed and filtered on the consumer side via the
        // ref map.
        if ((m.type == itch::MsgType::Add ||
             m.type == itch::MsgType::Action) &&
            symbols.count(std::string(m.stock, 8)) == 0) continue;
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
        std::fprintf(stderr, "usage: %s <file.itch> [SYMBOL...]\n", argv[0]);
        return 1;
    }
    std::string path = argv[1];
    std::unordered_set<std::string> symbols;
    for (int i = 2; i < argc; ++i) symbols.insert(pad8(argv[i]));
    if (symbols.empty()) symbols.insert(pad8("MBTEST"));

    uint64_t parsed = 0, matched = 0;
    std::thread feeder(feed_thread, path, symbols, &parsed, &matched);

    Venue<Stats> venue;
    itch::VenueBookBuilder book(1 << 17);   // +/- $13.11 around first price

    uint64_t applied = 0;
    auto t0 = Clock::now();
    itch::Message m;
    while (true) {
        if (!g_ring.try_pop(m)) {
            if (g_feed_done.load(std::memory_order_acquire) &&
                g_ring.size_approx() == 0) break;
            continue;
        }
        book.apply(venue, m);
        ++applied;
    }
    auto t1 = Clock::now();
    feeder.join();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("parsed:      %llu messages\n", (unsigned long long)parsed);
    std::printf("applied:     %llu messages across %zu book(s)"
                " (%llu dropped)\n",
                (unsigned long long)applied, venue.size(),
                (unsigned long long)book.dropped());
    std::printf("elapsed:     %.3f s (%.2f M msgs/s applied)\n", sec,
                applied / sec / 1e6);
    for (SymbolId s = 0; s < venue.size(); ++s) {
        const auto& e = venue.at(s);
        std::printf("%.8s  trades %llu", venue.name(s),
                    (unsigned long long)venue.handler(s).trades);
        if (e.has_bid() && e.has_ask()) {
            std::printf("  bid %.4f x ask %.4f  %zu open\n",
                        e.best_bid() / 10000.0, e.best_ask() / 10000.0,
                        e.open_orders());
        } else {
            std::printf("  one or both sides empty, %zu open\n",
                        e.open_orders());
        }
    }
    return 0;
}
