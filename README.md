# matchbook

A low-latency limit order book and matching engine in C++20, with a Nasdaq
ITCH 5.0 feed handler, a lock-free SPSC pipeline between the feed and
matching threads, and an Avellaneda-Stoikov market-making layer on top.

Zero external dependencies. Header-only core. Builds clean with
`-Wall -Wextra -Wpedantic -Wshadow` and runs clean under ASAN + UBSAN.

## Benchmarks

Measured on a single-core Linux container (GCC 13, `-O3 -march=native`),
mixed workload: 50% passive adds around a drifting mid, 40% cancels,
10% aggressive orders crossing the spread, on a pre-warmed book with
~150k resting orders.

```
throughput:     15.2 M ops/s        (10M-op run, 54% of ops produced fills)

per-op latency  (steady_clock, 2M samples, ~21 ns clock overhead included)
p50:            48 ns
p90:            101 ns
p99:            210 ns
p99.9:          474 ns

ITCH replay:    3.0 M msgs/s applied (2M-message file, full A/E/X/D/U lifecycle)
```

Run `./build/bench` yourself; numbers scale with your hardware. The SPSC
pipeline benchmark in particular needs >= 2 physical cores to mean anything.

## Design

```
 feed thread                          matching thread
┌─────────────────┐                 ┌──────────────────────────────────┐
│ ITCH 5.0 parser │   lock-free    │            MatchingEngine        │
│ (itch_parser)   │ ──SPSC ring──▶ │  ┌────────────────────────────┐  │
│ normalize msgs  │  (spsc_ring)   │  │ flat array of price levels │  │
└─────────────────┘                │  │ intrusive FIFO per level   │  │
                                   │  │ occupancy bitmap for best  │  │
        strategy tick              │  │ pooled Order nodes         │  │
┌─────────────────────┐            │  │ flat id -> Order* map      │  │
│ Avellaneda-Stoikov  │ ─quotes──▶ │  └────────────────────────────┘  │
│ (strategy/)         │            └──────────────────────────────────┘
└─────────────────────┘
```

The decisions that matter, and why:

**Flat array of price levels, not `std::map`.** Prices are integer ticks in
a bounded band, so a level lookup is one index computation into contiguous
memory. A red-black tree costs O(log n) pointer chases through
cache-hostile nodes on every touch; the array costs one predictable load.

**Occupancy bitmap for best-price recovery.** The one thing the flat array
makes awkward is "the best level just emptied, where's the next one?" A
per-side bitmap (one bit per level) answers it with masked `ctz`/`clz`
scans over 64-bit words: effectively O(1), and 20k price levels fit in
2.5 KB per side, i.e. a handful of cache lines.

**Intrusive doubly-linked FIFO per level.** Time priority is list order.
Cancels unlink in O(1) with no search, because the id map points straight
at the node. Nodes are never heap-allocated on the hot path; they come
from a chunked pool with a free list, so allocation is a pointer pop and
consecutive allocations are cache-adjacent.

**Flat vector for id lookup.** Order ids are engine-assigned and
sequential, so `id -> Order*` is a vector index, not a hash. Cancel and
modify are one indirection.

**Compile-time event handler policy.** The engine is templated on a
`Handler` (accept/trade/cancel/reject callbacks). No virtual dispatch, no
`std::function`, and a no-op handler compiles to nothing.

**Lock-free SPSC ring between feed and matching threads.** Single
producer, single consumer, cache-line-padded head/tail with each side
caching the other's index, so the common case is one relaxed load and one
release store per op. Backpressure is explicit: the producer spins when
the ring is full.

**Exchange-realistic modify semantics.** Reducing quantity at the same
price amends in place and keeps time priority; any price change or size
increase is cancel-replace, goes to the back of the queue, and can match
on re-entry. `reduce()` handles feed-driven partial executions/cancels
without losing queue position.

## Order semantics

| Operation       | Behavior                                                        |
|-----------------|-----------------------------------------------------------------|
| `submit_limit`  | Matches while crossed (price-time priority, executes at the resting order's price), rests the remainder |
| `submit_market` | Matches until filled or book exhausted; remainder is discarded  |
| `cancel`        | O(1) by id                                                      |
| `modify`        | Amend-down in place keeps priority; reprice/upsize is cancel-replace |
| `reduce`        | Feed-driven partial fill/cancel, keeps priority                 |

## Build and run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/tests                      # correctness suite (~193k checks)
./build/bench 10000000             # throughput + latency percentiles
./build/mmsim 100000 0.1           # AS vs symmetric market making
./build/itchgen /tmp/sample.itch 2000000
./build/replay /tmp/sample.itch MBTEST
```

Sanitizer build:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMATCHBOOK_SANITIZE=ON
cmake --build build-asan --target tests && ./build-asan/tests
```

### Replaying real Nasdaq data

`replay` reads the standard TotalView-ITCH 5.0 file format (2-byte
big-endian length prefix per message). Nasdaq publishes free full-day
sample files (emma.nasdaq.com); decompress one and run:

```bash
./build/replay 01302020.NASDAQ_ITCH50 AAPL
```

The feed thread parses and filters; the matching thread reconstructs the
book from the A/F/E/C/X/D/U lifecycle over the SPSC ring.

## Market-making layer

`strategy/avellaneda_stoikov.hpp` implements the closed-form
Avellaneda-Stoikov (2008) quotes: a reservation price skewed against
current inventory plus an optimal half-spread driven by risk aversion,
volatility, and time horizon. `mmsim` runs the agent against random
background flow and compares it with a naive symmetric quoter at the same
spread. Representative output (100k steps x 5 runs):

```
strategy       PnL(ticks)  final inv    max |inv|    fills
AS                 222807        370         1707    98893
symmetric          532573      10760        19221   123885
```

The symmetric quoter's larger mark-to-market PnL comes with ~11x the peak
inventory: it is mostly unhedged directional exposure, not edge. The AS
skew keeps inventory mean-reverting around zero, which is the entire point
of the model.

## Testing

`tests/test_engine.cpp` covers price-time priority, execution at the
maker's price, partial fills, market-order sweep and remainder discard,
cancel edge cases, amend-vs-replace priority semantics, modifies that
cross the book, band rejection, the bitmap, and the ring, plus a 200k-op
randomized fuzz that asserts book invariants (never locked or crossed,
consistent open-order accounting) after every operation. CI runs the
suite in Release and under ASAN + UBSAN.

## Roadmap

- IOC / FOK time-in-force
- MoldUDP64 framing for live multicast replay
- RL market-making agent trained against the simulator (AS as the baseline)
- pybind11 bindings for research/backtest workflows

## License

MIT
