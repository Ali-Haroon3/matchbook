# matchbook

A low-latency limit order book and matching engine in C++20, with a Nasdaq
ITCH 5.0 feed handler, a lock-free SPSC pipeline between the feed and
matching threads, and an Avellaneda-Stoikov market-making layer on top.

Zero external dependencies. Header-only core. Builds clean with
`-Wall -Wextra -Wpedantic -Wshadow` and runs clean under ASAN + UBSAN.
Optional pybind11 bindings for research workflows (off by default; the
core stays dependency-free).

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

**Two event-delivery modes, chosen at compile time.** By default the
handler is called directly, mid-match, with zero buffering — the fast
path. In that mode the engine is not re-entrant: a callback must not call
back into `submit_*`/`cancel`/`modify`/`reduce` (it would free the node
the matcher is holding), and a debug-build assert enforces it.
`ReentrantMatchingEngine<Handler>` (an alias for the same engine with
`DeferEvents = true`) instead buffers events during an operation and
dispatches them only once it completes, so callbacks run against a fully
consistent book and may freely re-enter — a strategy can place an order
straight from inside `on_trade`. Events arrive in the same order and still
before the call returns; the only cost is one buffer push/pop per event,
which is why it is opt-in and the default stays allocation-free.

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
| &nbsp;&nbsp;`IOC` / `FOK` | IOC cancels the unfilled remainder instead of resting it; FOK pre-checks depth at-or-better than the limit (bitmap hop over level totals) and fills completely or executes nothing |
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
./build/mmsim 100000 0.1           # AS vs symmetric vs Q-learning market making
./build/itchgen /tmp/sample.itch 2000000
./build/replay /tmp/sample.itch MBTEST
./build/mcast recv 239.192.0.1 26400 MBTEST 127.0.0.1 &   # live receiver
./build/mcast send /tmp/sample.itch 239.192.0.1 26400 5000 127.0.0.1
```

Sanitizer build:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMATCHBOOK_SANITIZE=ON
cmake --build build-asan --target tests && ./build-asan/tests
```

### Python bindings

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMATCHBOOK_PYTHON=ON  # fetches pybind11
cmake --build build --target pymatchbook -j
PYTHONPATH=build python3
```

```python
import matchbook as mb
e = mb.Engine(1, 10000)
e.submit_limit(mb.Side.Sell, 100, 5)
e.submit_limit(mb.Side.Buy, 100, 8, mb.TimeInForce.IOC)
e.take_trades()   # [Trade(taker=2, maker=1, price=100, qty=5)]
```

The module exposes the engine (submit/cancel/modify/reduce, book state,
a drainable trade list), the Avellaneda-Stoikov quoter, and the
Q-learning quoter — enough to drive backtests or train policies from
Python. `tests/test_bindings.py` is the smoke test; CI runs it.

### Replaying real Nasdaq data

`replay` reads the standard TotalView-ITCH 5.0 file format (2-byte
big-endian length prefix per message). For the wire transport,
`mold_udp64.hpp` implements MoldUDP64 framing: packet encode/decode
(including heartbeats and end-of-session) plus a `SequenceTracker` that
skips duplicate blocks and accounts for gaps across lossy/reordered
packets. Nasdaq publishes free full-day
sample files (emma.nasdaq.com); decompress one and run:

```bash
./build/replay 01302020.NASDAQ_ITCH50 AAPL
```

The feed thread parses and filters; the matching thread reconstructs the
book from the A/F/E/C/X/D/U lifecycle over the SPSC ring.

`mcast` is the live wire-transport counterpart: `mcast send` frames an
ITCH file into MTU-sized MoldUDP64 packets and multicasts them at a paced
rate (ending with an end-of-session burst), and `mcast recv` joins the
group, runs the codec's duplicate/gap accounting, and reconstructs the
book through the same SPSC feed/matching pipeline as `replay` (the
book-building logic is shared, `itch_book_builder.hpp`). The interface
address argument selects the NIC, as production multicast feeds do;
`127.0.0.1` runs the whole loop on loopback, where the received book is
bit-identical to the file replay's. A receiver that misses the
end-of-session packet exits after 5s of feed silence instead of hanging.

## Market-making layer

`strategy/avellaneda_stoikov.hpp` implements the closed-form
Avellaneda-Stoikov (2008) quotes: a reservation price skewed against
current inventory plus an optimal half-spread driven by risk aversion,
volatility, and time horizon.

`strategy/rl_quoter.hpp` is a tabular Q-learning market maker trained
against the same simulator, with AS as the baseline. State is the current
inventory in coarse buckets; an action picks the bid and ask offsets from
mid independently (the widest offset sits past the background-flow band,
so it doubles as "pull this side"); reward is the per-step mark-to-market
change minus a quadratic inventory penalty. The whole policy is a
61-state x 16-action table -- no function approximation, no external
dependencies, trains in seconds.

`mmsim` trains the Q-learner (15 runs, epsilon annealed to zero, seeds
disjoint from evaluation), then compares all three quoters on held-out
seeds. Representative output (100k steps x 5 runs):

```
strategy       PnL(ticks)  final inv    max |inv|    fills
AS                 224866        412         1600    98964
symmetric          563338      10139        18980   123912
RL(Q)              526373         69          154    84120
```

The symmetric quoter's larger mark-to-market PnL comes with ~12x the peak
inventory of AS: it is mostly unhedged directional exposure, not edge. The
AS skew keeps inventory mean-reverting around zero, which is the entire
point of the model. The Q-learner rediscovers that skew from reward alone
-- asymmetric offsets when inventoried, pulling a side when it gets long
or short -- and in this sim beats AS on both PnL and peak inventory,
mostly because it also learns to quote tighter than the closed form
(whose arrival-rate assumptions don't match this flow) dares to.

## Testing

`tests/test_engine.cpp` covers price-time priority, execution at the
maker's price, partial fills, market-order sweep and remainder discard,
cancel edge cases, amend-vs-replace priority semantics and the
cancel-replace event pair, modifies that cross the book, IOC/FOK
time-in-force, band rejection, a rejected feed replace keeping its order
reachable, the bitmap, MoldUDP64 framing (round trip, control packets,
malformed input, gap tracking), the ring, and the Q-learning quoter
(bucketing bounds, update math, uncrossed quotes), and the re-entrant
engine (event-tape parity with the default mode, a handler that cancels
the maker from inside on_trade, a handler that re-submits from a fill),
plus a 200k-op randomized fuzz (limits, cancels, markets, IOC/FOK) — run
against both the default and deferred-event engines — that asserts book
invariants (never locked or crossed, consistent open-order accounting)
after every operation. CI runs the suite in Release and under ASAN + UBSAN.

## Roadmap

- ~~Live multicast replay tool (UDP receiver over the MoldUDP64 codec)~~ done: `tools/mcast_main.cpp`
- ~~RL market-making agent trained against the simulator (AS as the baseline)~~ done: `strategy/rl_quoter.hpp`
- ~~pybind11 bindings for research/backtest workflows~~ done: `bindings/py_matchbook.cpp` (opt-in, `-DMATCHBOOK_PYTHON=ON`)

## License

MIT
