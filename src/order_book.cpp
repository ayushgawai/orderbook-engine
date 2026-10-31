#include "ob/order_book.hpp"

#include <limits>
#include <stdexcept>

namespace ob {

OrderBook::OrderBook(std::size_t expected_orders) { index_.reserve(expected_orders); }

template <typename Book>
void OrderBook::match(Book& book, OrderId taker, Price limit, bool is_buy, Qty& remaining, std::vector<Fill>& fills) {
  while (remaining > 0 && !book.empty()) {
    auto lvl = book.begin();
    const Price p = lvl->first;
    if (is_buy ? p > limit : p < limit) break;  // best opposite price no longer marketable
    Queue& q = lvl->second.queue;
    while (remaining > 0 && !q.empty()) {
      Order& maker = q.front();
      const Qty traded = std::min(remaining, maker.qty);
      fills.push_back({maker.id, taker, p, traded});
      remaining -= traded;
      maker.qty -= traded;
      lvl->second.total -= traded;
      if (maker.qty == 0) {
        index_.erase(maker.id);
        q.pop_front();
      }
    }
    if (q.empty()) book.erase(lvl);
  }
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  if (side == Side::Buy) {
    auto& lvl = bids_[price];
    lvl.queue.push_back({id, qty});
    lvl.total += qty;
    index_[id] = {side, price, std::prev(lvl.queue.end())};
  } else {
    auto& lvl = asks_[price];
    lvl.queue.push_back({id, qty});
    lvl.total += qty;
    index_[id] = {side, price, std::prev(lvl.queue.end())};
  }
}

std::vector<Fill> OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif) {
  if (qty <= 0) throw std::invalid_argument("quantity must be positive");
  if (index_.count(id)) throw std::invalid_argument("duplicate order id");
  std::vector<Fill> fills;
  Qty remaining = qty;
  if (side == Side::Buy)
    match(asks_, id, price, true, remaining, fills);
  else
    match(bids_, id, price, false, remaining, fills);
  if (remaining > 0 && tif == TimeInForce::GTC) rest(id, side, price, remaining);
  return fills;
}

std::vector<Fill> OrderBook::add_market(OrderId id, Side side, Qty qty) {
  const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
  return add_limit(id, side, limit, qty, TimeInForce::IOC);
}

bool OrderBook::cancel(OrderId id) {
  auto found = index_.find(id);
  if (found == index_.end()) return false;
  const Locator loc = found->second;
  auto drop = [&](auto& book) {
    auto lvl = book.find(loc.price);
    lvl->second.total -= loc.it->qty;
    lvl->second.queue.erase(loc.it);
    if (lvl->second.queue.empty()) book.erase(lvl);
  };
  if (loc.side == Side::Buy)
    drop(bids_);
  else
    drop(asks_);
  index_.erase(found);
  return true;
}

std::vector<Fill> OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
  auto found = index_.find(id);
  if (found == index_.end()) return {};
  Locator& loc = found->second;
  if (new_price == loc.price && new_qty <= loc.it->qty && new_qty > 0) {
    const Qty delta = loc.it->qty - new_qty;
    loc.it->qty = new_qty;
    if (loc.side == Side::Buy)
      bids_.find(loc.price)->second.total -= delta;
    else
      asks_.find(loc.price)->second.total -= delta;
    return {};  // in-place reduction keeps time priority
  }
  const Side side = loc.side;
  cancel(id);
  if (new_qty <= 0) return {};
  return add_limit(id, side, new_price, new_qty);
}

std::optional<Price> OrderBook::best_bid() const {
  return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
}

std::optional<Price> OrderBook::best_ask() const {
  return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
}

std::vector<Level> OrderBook::depth(Side side, std::size_t levels) const {
  std::vector<Level> out;
  auto collect = [&](const auto& book) {
    for (const auto& [p, lvl] : book) {
      if (out.size() == levels) break;
      out.push_back({p, lvl.total, static_cast<int>(lvl.queue.size())});
    }
  };
  if (side == Side::Buy)
    collect(bids_);
  else
    collect(asks_);
  return out;
}

bool OrderBook::crossed() const {
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}

}  // namespace ob
