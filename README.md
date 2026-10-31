# Order Book Engine

A single-instrument **limit order book and matching engine** in C++20 with price-time priority. It supports limit, market and IOC orders, O(1) cancel, and priority-aware modify. Correctness is checked against a naive reference implementation on 20,000 randomized operations, and a replay benchmark reports throughput and tail latency.

## Design

| Piece | Choice | Why |
|---|---|---|
| Prices | `int64` ticks | no floating-point comparison at the matching boundary |
| Levels | `std::map` per side (bids descending, asks ascending) | best price is `begin()`, in-order depth walks |
| Queue per level | FIFO `std::list` | stable iterators, so orders can be removed from the middle |
| Order index | `unordered_map<id, (side, price, iterator)>`, pre-reserved | O(1) cancel and modify without searching the level |
| Matching | aggressive order walks opposite levels while marketable, filling makers in FIFO order | price-time priority |
| Modify | quantity reduction in place keeps queue position. Size increase or reprice is cancel plus re-add at the back | the usual exchange rule, and it prevents queue jumping |
| Safety | rejects non-positive quantity and duplicate ids. A test asserts the book is never crossed after any operation | |

## Build, test, benchmark

```bash
cmake -S . -B build && cmake --build build -j
build/ob_tests            # unit tests + 20,000 randomized ops differential-tested against a reference book
build/ob_bench 2000000    # replay benchmark
```

Benchmark on a 4-vCPU x86 container with `-O2`, single thread, 2M messages. The mix is 35% cancels, 5% marketable orders and 60% passive adds within 15 ticks of the touch:

```
messages 2000000, fills 119175, resting at end 490109
throughput 4.68 M msgs/s
latency ns  p50 136  p90 491  p99 900  p99.9 4156
```

Latency is sampled on 1 in 16 messages with `steady_clock`, so it includes clock overhead. Across runs on the shared container, throughput varied between 3.9 and 4.7 M msgs/s, and p99.9 varied the most.

## Where the time goes, and next steps

- `std::list` and `std::map` allocate per order and per new level, which hurts cache locality and drives the p99.9 tail. The next step is a pooled intrusive list with a free-list allocator.
- A price-indexed array of levels around the touch would replace the map for O(1) best-price updates.
- Batch fills into a preallocated buffer instead of returning `std::vector`.
- Add a multi-instrument gateway with a lock-free SPSC queue between parser and matcher.

## Layout

```
include/ob/order_book.hpp   public API and data structures
src/order_book.cpp          matching, rest, cancel, modify, depth
tests/test_order_book.cpp   priority, partial fills, IOC, market sweep, modify rules, randomized reference test
bench/bench.cpp             message-stream replay with latency percentiles
```

MIT License.
