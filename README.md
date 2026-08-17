# matchbook

A low-latency limit order book and matching engine in C++20, with a Nasdaq
ITCH 5.0 feed handler, a lock-free SPSC pipeline between the feed and
matching threads, and an Avellaneda-Stoikov market-making layer on top.
The engine speaks the full exchange order-type zoo -- IOC/FOK/post-only,
icebergs, stops and stop-limits, self-trade prevention -- runs call
auctions with an equilibrium-price uncross next to continuous trading,
and closes the loop as a venue: OUCH-style order entry in, ITCH 5.0
market data out, with a round-trip test proving a consumer of the
published feed reconstructs the identical displayed book.

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

**48-byte nodes, cold state off to the side.** Owner, flags, and a side-
table index ride in what used to be node padding (a `static_assert`
holds the line at 48 bytes). Order types that need more -- an iceberg's
reserve and peak, a stop-limit's post-trigger price -- keep it in a
pooled side table that plain limits never touch, so the common path's
cache density is unchanged by the exotic order types existing.

**Stop books mirror the price books.** Pending stops live in per-side
flat Level arrays + occupancy bitmaps keyed by trigger price (allocated
lazily; engines that never see a stop pay nothing), so "which stop does
this print arm next" is the same masked bitmap scan as best-price
recovery, and triggering pops FIFO from the level head. After every
operation the engine pumps triggers to fixpoint -- a stop run is a loop,
not a recursion, and it behaves identically under deferred events.

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
| &nbsp;&nbsp;`IOC` / `FOK` | IOC cancels the unfilled remainder instead of resting it; FOK pre-checks executable depth at-or-better than the limit (bitmap hop over level totals, hidden reserve included) and fills completely or executes nothing |
| &nbsp;&nbsp;`PostOnly` | Never takes liquidity: killed on entry if it would lock or cross the opposite side, otherwise rests like GTC |
| `submit_iceberg` | Rests showing at most `display`; an exhausted clip replenishes from the hidden reserve and re-queues at the back of the level (each clip earns its own time priority). Depth shows displayed qty only |
| `submit_stop` / `submit_stop_limit` | Parked off-book until the last trade reaches the trigger (buy: `last >= stop`, sell: `last <= stop`), then a market order / GTC limit. Cascades run to fixpoint: one stop's fills can arm the next, buy stops firing in ascending trigger order, sell stops descending |
| `submit_market` | Matches until filled or book exhausted; remainder is discarded  |
| `cancel`        | O(1) by id; works on resting orders and pending stops alike     |
| `modify`        | Amend-down in place keeps priority (icebergs shave reserve first); reprice/upsize is cancel-replace and keeps owner, STP policy, and iceberg peak |
| `reduce`        | Feed-driven partial fill/cancel, keeps priority                 |

Every submit takes an optional participant `owner` (nonzero arms
**self-trade prevention**) and an `StpPolicy` carried by the incoming
order: `CancelTaker` (default) stops the match and kills the incoming
remainder, `CancelMaker` cancels the stale resting order and keeps
matching, `CancelBoth` does both. Anonymous flow (`owner == 0`) pays one
loop-invariant compare per fill for the feature's existence.

**Auctions.** `halt()` opens a call phase: GTC limits and icebergs
accumulate without matching (the book may cross), IOC/FOK/post-only are
killed on entry, market orders are rejected, stops park without
triggering. `uncross()` clears the overlap at a single equilibrium price
-- maximum executed volume, ties broken by least imbalance, then
proximity to the last trade, then the lowest price -- filling both sides
in price-time priority with hidden reserve participating in full.
`resume()` returns to continuous trading and fires whatever stops the
opening prints armed. `top_levels()` / `visit_levels()` provide
best-first aggregated depth (price, displayed qty, order count) for
market-data publication; `hidden_at()`, `stop_depth_at()`,
`pending_stops()`, and `last_trade()` expose the rest of the state.

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

e.submit_iceberg(mb.Side.Sell, 101, 500, 25)     # 25 lit, 475 dark
e.submit_stop(mb.Side.Buy, 103, 50)              # fires when 103 prints
e.submit_limit(mb.Side.Buy, 101, 8, owner=7)     # self-trade prevented
e.top_levels(mb.Side.Sell, 5)                    # [LevelView(...), ...]

e.halt(); e.submit_limit(mb.Side.Buy, 102, 30)   # call phase
e.uncross(); e.resume()                          # opening cross
```

The module exposes the engine (submit/cancel/modify/reduce across the
full order-type zoo, self-trade prevention, auctions, book and stop-book
state, drainable trade and cancel lists), the Avellaneda-Stoikov quoter,
and the Q-learning quoter — enough to drive backtests or train policies
from Python. `tests/test_bindings.py` is the smoke test; CI runs it. An
installed pybind11 (`pip install pybind11`) is preferred over the
FetchContent fallback, so offline builds work too.

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

## Market data out (ITCH publisher)

`itch_publisher.hpp` is the other half of the venue: a `Publisher`
wraps the engine and publishes its life as spec-layout ITCH 5.0
(`itch_encode.hpp` holds the encoders), describing the *displayed*
book. Resting quantity and every replenished iceberg clip go out as
`A` with a fresh reference; continuous fills as maker-side `E`; auction
fills as `C` at the clearing price plus one aggregate `Q` cross whose
volume is where hidden participation shows up; silent in-place amends
as `X`/`D` diffed across the call; halts as `H` trading actions, which
`BookBuilder` now honors so a consumer's book may stand crossed through
a call phase exactly like the venue's. Kills, rejects, and pending
stops publish nothing -- they never touched the displayed book.

The property that keeps this honest: the round-trip test drives 40k
random operations spanning every order type and auction cycles, parses
the published stream with the repo's own parser, applies it through
`BookBuilder` into a second engine, and requires that book to match the
source level-for-level -- prices, quantities, order counts, best
prices -- continuously. The publisher needs exactly two engine hooks:
an optional compile-time `on_rest` handler callback (accept announces
intent; only rest changes the displayed book) and an `order_qty`
accessor for diffing silent amends.

**Late joiners** get a GLIMPSE-style snapshot: `Publisher::snapshot()`
serializes the displayed book as out-of-band Add messages under the
live references (leading with the halt state, since a call-phase book
legitimately stands crossed) and returns the sequence to splice into
the live stream at; `mold::SequenceTracker` takes that starting
sequence, so replayed pre-join packets skip as already-seen rather than
double-applying or counting as gaps. The test joins mid-stream, inside
a halt, off snapshot + full-wire replay, and tracks the source book
level-for-level to the end.

## Multi-symbol venues

`venue.hpp` runs one engine per symbol -- each with its own price band
and its own Handler instance, so event streams stay attributable --
registered explicitly, because the flat per-symbol level arrays cost
band x 2 x sizeof(Level) and that belongs in the caller's hands, not
hidden behind whole-tape magic. `itch::VenueBookBuilder` routes a mixed
feed across the books via one day-unique ref map, honors per-symbol
trading actions, and offers an auto-band mode (register a symbol on its
first Add, banded around that price; later out-of-band adds count as
dropped instead of growing books without bound). `replay` now takes
multiple symbols and builds them side by side.

## Pre-trade risk (market access gate)

`risk_gate.hpp` is the 15c3-5-shaped layer: a `RiskGate` owns the
engine, mirrors its order-entry surface, and refuses flow before the
engine sees it -- kill switch (with `kill_and_cancel_all()` flattening
resting orders and pending stops), a clockless per-window message
budget, an optional market-order ban, per-order quantity and notional
caps, a price collar around the last trade on every price that can
execute, an open-order cap, and a per-owner net-filled position limit
with a worst-case pre-check fed by the gate's own fill tracking.
Rejections are synchronous return values with recorded reasons and
counters. Two absolutes match how real gates behave: risk *reduction*
(cancel/reduce) is never blocked, and a default-limits gate is a pure
passthrough -- pinned by an event-tape parity test against a bare
engine.

## Order entry (OUCH-style)

`ouch.hpp` is the client-facing counterpart to the ITCH side: an
OUCH 4.2-shaped binary order-entry protocol (fixed-width big-endian
fields, one-letter types, 14-byte space-padded client tokens that are
single-use for the session) plus a session `Gateway` that wires the
message stream to an engine. Enter/Replace/Cancel go in;
Accepted/Executed/Canceled/Replaced/Rejected come out, one byte vector
per message — exactly the shape `mold::encode` takes, so gating OUCH
responses onto a MoldUDP64 wire is one call.

The gateway enforces the protocol's session rules rather than leaving
them to the engine: token reuse (including a replaced-away or dead
token) is rejected, a replace binds its new token before re-entry so
fills print under it — with the Replaced ack inserted ahead of them in
the outbound stream — and cancels carry a reason (user-requested,
killed-on-entry for IOC/FOK/post-only/STP kills, self-trade for STP
removals of resting orders). Both sides of a fill get an Executed with
a shared match number. Deviations from Nasdaq's spec are deliberate and
documented in the header: 8-byte signed tick prices, the engine's TIF
enum, and Enter carrying display/owner/STP so icebergs and self-trade
prevention are reachable over the wire.

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
cancel-replace event pair, modifies that cross the book, IOC/FOK and
post-only time-in-force, self-trade prevention (all three policies,
markets, an FOK truncated by STP, policy surviving cancel-replace),
icebergs (clip replenishment and re-queue priority, hidden-inclusive FOK,
reserve-first reduce/amend, peak surviving reprice), stops (pending
visibility, trigger-on-entry, stop-limits resting their remainder,
cascade chains, ascending/descending fire order, cancel-only lifecycle),
auctions (call-phase accumulation, the equilibrium tie-break ladder,
iceberg participation, stops arming off the opening print, unbalanced
crosses), the depth snapshot, band rejection, a rejected feed replace
keeping its order reachable, the bitmap, MoldUDP64 framing (round trip,
control packets, malformed input, gap tracking), the OUCH codec and
gateway (round trips, token lifecycle, replace-ack ordering ahead of
re-entry fills, cancel reasons, STP and icebergs over the wire, framing
outbound into Mold), the ring, and the
Q-learning quoter (bucketing bounds, update math, uncrossed quotes), and
the re-entrant engine (event-tape parity with the default mode, a handler
that cancels the maker from inside on_trade, a handler that re-submits
from a fill), the ITCH encoders and publisher (mapping unit tests plus
the 40k-op round-trip rebuild above), the snapshot/late-join splice
(including a mid-halt join off a crossed book), the multi-symbol venue
and its routing builder, the risk gate (every limit tripped and the
default-limits passthrough parity), plus two randomized fuzzes — run
against both the default and deferred-event engines — throwing the full
order-type zoo at the book (200k ops) and driving halt/uncross/resume
cycles under random flow (60k ops), asserting the book is never locked
or crossed and the accounting comes out clean after every operation.

On top of the invariant fuzzes sits a **differential fuzz**:
`tests/reference_engine.hpp` is a second, deliberately naive
implementation of the entire public contract — `std::map`, `std::deque`,
linear scans, every order type, STP, stops, auctions — and 75k lockstep
operations require the fast engine to agree with it on every return
value, every event in order with all fields, and the whole book state
(levels, hidden, counts, last trade, stop and open-order accounting)
throughout. Mutation-checked: breaking FIFO insertion or stop fire
order makes it fail by the hundreds of thousands of checks. CI runs the
suite in Release and under ASAN + UBSAN.

## Roadmap

- ~~Live multicast replay tool (UDP receiver over the MoldUDP64 codec)~~ done: `tools/mcast_main.cpp`
- ~~RL market-making agent trained against the simulator (AS as the baseline)~~ done: `strategy/rl_quoter.hpp`
- ~~pybind11 bindings for research/backtest workflows~~ done: `bindings/py_matchbook.cpp` (opt-in, `-DMATCHBOOK_PYTHON=ON`)
- ~~Post-only time-in-force~~ done: `TimeInForce::PostOnly`
- ~~Self-trade prevention (cancel-taker / cancel-maker / cancel-both)~~ done: per-order `owner` + `StpPolicy`
- ~~Iceberg orders with hidden reserve~~ done: `submit_iceberg`
- ~~Stop and stop-limit orders with cascading triggers~~ done: `submit_stop`, `submit_stop_limit`
- ~~Call auctions: halt / equilibrium-price uncross / resume~~ done: `halt()`, `uncross()`, `resume()`
- ~~L2 depth snapshots with per-level order counts~~ done: `top_levels()`, `visit_levels()`
- ~~OUCH-style binary order entry with a token-tracking session gateway~~ done: `ouch.hpp`
- ~~Outbound ITCH publisher with a feed-consumer round-trip proof~~ done: `itch_publisher.hpp`, `itch_encode.hpp`
- ~~Differential fuzz against a naive reference implementation~~ done: `tests/reference_engine.hpp`
- ~~Multi-symbol venue with a routing book builder~~ done: `venue.hpp`, `itch::VenueBookBuilder`
- ~~Pre-trade risk gate (15c3-5-style market access controls)~~ done: `risk_gate.hpp`
- ~~Snapshot/late-join recovery (GLIMPSE-style)~~ done: `Publisher::snapshot()`, spliced `SequenceTracker`

## License

MIT
