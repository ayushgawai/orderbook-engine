// Replays a synthetic message stream (adds near the touch, cancels, marketable orders) and reports
// throughput and per-message latency percentiles.   ob_bench [messages]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "ob/order_book.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

struct Msg {
  int type;  // 0 add, 1 cancel, 2 market
  OrderId id;
  Side side;
  Price px;
  Qty qty;
};

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
  std::mt19937_64 rng(7);
  std::vector<Msg> msgs;
  msgs.reserve(n);
  std::vector<OrderId> live;
  OrderId next = 1;
  for (std::size_t i = 0; i < n; ++i) {
    int r = rng() % 100;
    if (r < 35 && !live.empty()) {  // cancels dominate real feeds
      auto k = rng() % live.size();
      msgs.push_back({1, live[k], Side::Buy, 0, 0});
      live[k] = live.back();
      live.pop_back();
    } else if (r < 40) {
      msgs.push_back({2, next++, rng() % 2 ? Side::Buy : Side::Sell, 0, 1 + static_cast<Qty>(rng() % 20)});
    } else {
      Side s = rng() % 2 ? Side::Buy : Side::Sell;
      Price offset = static_cast<Price>(rng() % 15);
      Price px = s == Side::Buy ? 10'000 - offset : 10'001 + offset;  // passive, within 15 ticks of touch
      msgs.push_back({0, next, s, px, 1 + static_cast<Qty>(rng() % 100)});
      live.push_back(next++);
    }
  }

  OrderBook book(1 << 20);
  std::vector<double> lat;
  lat.reserve(n / 16 + 1);
  std::size_t fills = 0;
  auto t0 = Clock::now();
  for (std::size_t i = 0; i < n; ++i) {
    const Msg& m = msgs[i];
    const bool sample = (i & 15) == 0;  // time 1 in 16 messages to keep clock overhead small
    auto s = sample ? Clock::now() : Clock::time_point{};
    if (m.type == 0)
      fills += book.add_limit(m.id, m.side, m.px, m.qty).size();
    else if (m.type == 1)
      book.cancel(m.id);
    else
      fills += book.add_market(m.id, m.side, m.qty).size();
    if (sample) lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - s).count());
  }
  double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return lat[static_cast<std::size_t>(p * (lat.size() - 1))]; };
  std::printf("messages %zu, fills %zu, resting at end %zu\n", n, fills, book.resting_orders());
  std::printf("throughput %.2f M msgs/s\n", n / secs / 1e6);
  std::printf("latency ns  p50 %.0f  p90 %.0f  p99 %.0f  p99.9 %.0f\n", pct(0.5), pct(0.9), pct(0.99), pct(0.999));
  return 0;
}
