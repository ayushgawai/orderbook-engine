#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <random>
#include <vector>

#include "ob/order_book.hpp"

using namespace ob;

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static void price_time_priority() {
  OrderBook b;
  b.add_limit(1, Side::Sell, 101, 5);
  b.add_limit(2, Side::Sell, 100, 5);
  b.add_limit(3, Side::Sell, 100, 5);  // same price, later: fills after #2
  auto f = b.add_limit(10, Side::Buy, 101, 12);
  CHECK(f.size() == 3);
  CHECK(f[0].maker == 2 && f[0].price == 100 && f[0].qty == 5);
  CHECK(f[1].maker == 3 && f[1].qty == 5);
  CHECK(f[2].maker == 1 && f[2].price == 101 && f[2].qty == 2);
  CHECK(b.best_ask() == 101 && b.depth(Side::Sell, 1)[0].total == 3);
  CHECK(!b.best_bid());
}

static void remainder_rests_and_ioc_does_not() {
  OrderBook b;
  b.add_limit(1, Side::Sell, 100, 3);
  b.add_limit(2, Side::Buy, 100, 5);
  CHECK(b.best_bid() == 100 && b.depth(Side::Buy, 1)[0].total == 2);
  b.add_limit(3, Side::Sell, 105, 4);
  auto f = b.add_limit(4, Side::Buy, 104, 10, TimeInForce::IOC);
  CHECK(f.empty() && b.resting_orders() == 2);
}

static void market_order_sweeps_levels() {
  OrderBook b;
  for (int i = 0; i < 5; ++i) b.add_limit(i + 1, Side::Buy, 100 - i, 10);
  auto f = b.add_market(99, Side::Sell, 35);
  CHECK(f.size() == 4 && f.back().price == 97 && f.back().qty == 5);
  CHECK(b.best_bid() == 97 && b.depth(Side::Buy, 1)[0].total == 5);
}

static void cancel_and_modify() {
  OrderBook b;
  b.add_limit(1, Side::Buy, 100, 10);
  b.add_limit(2, Side::Buy, 100, 10);
  CHECK(b.cancel(1) && !b.cancel(1));
  b.add_limit(3, Side::Buy, 100, 10);
  b.modify(2, 100, 4);  // reduce: keeps priority ahead of #3
  auto f = b.add_market(9, Side::Sell, 5);
  CHECK(f[0].maker == 2 && f[0].qty == 4 && f[1].maker == 3 && f[1].qty == 1);
  b.add_limit(4, Side::Buy, 100, 1);
  b.modify(3, 100, 20);  // increase from 9: loses priority to #4
  f = b.add_market(10, Side::Sell, 1);
  CHECK(f[0].maker == 4);
  f = b.modify(3, 105, 20);  // reprice through an empty ask side: rests at 105
  CHECK(f.empty() && b.best_bid() == 105);
}

static void rejects_bad_input() {
  OrderBook b;
  bool threw = false;
  try { b.add_limit(1, Side::Buy, 100, 0); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  b.add_limit(1, Side::Buy, 100, 1);
  threw = false;
  try { b.add_limit(1, Side::Sell, 101, 1); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

// Naive reference: a flat vector scanned in priority order. Slow but obviously correct.
struct Ref {
  struct O { OrderId id; Side side; Price px; Qty qty; long seq; };
  std::vector<O> book;
  long seq = 0;
  std::vector<Fill> add(OrderId id, Side s, Price px, Qty q, bool ioc) {
    std::vector<Fill> fills;
    while (q > 0) {
      int best = -1;
      for (int i = 0; i < (int)book.size(); ++i) {
        auto& o = book[i];
        if (o.side == s) continue;
        if (s == Side::Buy ? o.px > px : o.px < px) continue;
        if (best < 0) { best = i; continue; }
        auto& bo = book[best];
        bool better = s == Side::Buy ? (o.px < bo.px || (o.px == bo.px && o.seq < bo.seq))
                                     : (o.px > bo.px || (o.px == bo.px && o.seq < bo.seq));
        if (better) best = i;
      }
      if (best < 0) break;
      Qty t = std::min(q, book[best].qty);
      fills.push_back({book[best].id, id, book[best].px, t});
      q -= t;
      if ((book[best].qty -= t) == 0) book.erase(book.begin() + best);
    }
    if (q > 0 && !ioc) book.push_back({id, s, px, q, seq++});
    return fills;
  }
  bool cancel(OrderId id) {
    for (auto it = book.begin(); it != book.end(); ++it)
      if (it->id == id) { book.erase(it); return true; }
    return false;
  }
};

static void randomized_against_reference() {
  std::mt19937_64 rng(2026);
  OrderBook b;
  Ref r;
  std::vector<OrderId> live;
  for (OrderId id = 1; id <= 20000; ++id) {
    int op = rng() % 10;
    if (op < 2 && !live.empty()) {
      auto k = rng() % live.size();
      CHECK(b.cancel(live[k]) == r.cancel(live[k]));
      live.erase(live.begin() + k);
      continue;
    }
    Side s = rng() % 2 ? Side::Buy : Side::Sell;
    Price px = 1000 + static_cast<Price>(rng() % 21) - 10;
    Qty q = 1 + rng() % 50;
    bool ioc = op == 9;
    auto f1 = b.add_limit(id, s, px, q, ioc ? TimeInForce::IOC : TimeInForce::GTC);
    auto f2 = r.add(id, s, px, q, ioc);
    bool same = f1.size() == f2.size();
    for (size_t i = 0; same && i < f1.size(); ++i)
      same = f1[i].maker == f2[i].maker && f1[i].price == f2[i].price && f1[i].qty == f2[i].qty;
    CHECK(same);
    CHECK(!b.crossed());
    if (!ioc) live.push_back(id);
    if (failures) return;
  }
  CHECK(b.resting_orders() == r.book.size());
}

int main() {
  price_time_priority();
  remainder_rests_and_ioc_does_not();
  market_order_sweeps_levels();
  cancel_and_modify();
  rejects_bad_input();
  randomized_against_reference();
  std::printf(failures ? "%d failures\n" : "all tests passed (incl. 20000 randomized ops vs reference)\n", failures);
  return failures ? 1 : 0;
}
