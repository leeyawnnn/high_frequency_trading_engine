// Phase 6: HFT-shaped flat-array order book, verified against a slow std::map
// reference implementation.
#include "test_harness.hpp"
#include "hft/book_view.hpp"
#include "hft/itch_message.hpp"

#include <cstdint>
#include <map>
#include <random>

namespace {

// Deliberately slow, obviously-correct reference: a std::map per side. This is
// exactly the kind of book the flat array replaces; we trust it and check the
// fast book against it.
class ReferenceBook {
 public:
  void apply(const hft::ItchMessage& m) {
    auto& book = (m.side == static_cast<std::uint8_t>(hft::Side::kBuy)) ? bids_ : asks_;
    auto& lvl = book[m.price];
    switch (static_cast<hft::MsgType>(m.type)) {
      case hft::MsgType::kAdd:
        lvl += m.size;
        break;
      case hft::MsgType::kExecute:
      case hft::MsgType::kCancel:
        lvl = (m.size >= lvl) ? 0u : lvl - m.size;
        break;
      case hft::MsgType::kDelete:
        lvl = 0;
        break;
      default:
        break;
    }
    if (lvl == 0) book.erase(m.price);
  }

  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  std::int64_t best_bid_price() const { return bids_.rbegin()->first; }   // highest
  std::int64_t best_ask_price() const { return asks_.begin()->first; }    // lowest
  std::uint32_t best_bid_size() const { return bids_.rbegin()->second; }
  std::uint32_t best_ask_size() const { return asks_.begin()->second; }

 private:
  std::map<std::int64_t, std::uint32_t> bids_;
  std::map<std::int64_t, std::uint32_t> asks_;
};

template <std::size_t N>
void check_match(const hft::BookView<N>& fast, const ReferenceBook& ref) {
  CHECK_EQ(fast.has_bid(), ref.has_bid());
  CHECK_EQ(fast.has_ask(), ref.has_ask());
  if (ref.has_bid()) {
    CHECK_EQ(fast.best_bid_price(), ref.best_bid_price());
    CHECK_EQ(fast.best_bid_size(), ref.best_bid_size());
  }
  if (ref.has_ask()) {
    CHECK_EQ(fast.best_ask_price(), ref.best_ask_price());
    CHECK_EQ(fast.best_ask_size(), ref.best_ask_size());
  }
}

hft::ItchMessage mk(hft::MsgType t, hft::Side s, std::int64_t price, std::uint32_t size) {
  hft::ItchMessage m{};
  m.type = static_cast<std::uint8_t>(t);
  m.side = static_cast<std::uint8_t>(s);
  m.price = price;
  m.size = size;
  return m;
}

}  // namespace

HFT_TEST(basic_top_of_book) {
  const std::int64_t base = hft::price_from_double(99.00);
  const std::int64_t tick = hft::kPriceScale / 100;  // 1 cent
  hft::BookView<4096> b(base, tick);

  CHECK(!b.has_bid());
  CHECK(!b.has_ask());

  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(99.98), 300));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(99.99), 500));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(100.01), 200));

  CHECK(b.has_bid());
  CHECK(b.has_ask());
  CHECK_EQ(b.best_bid_price(), hft::price_from_double(99.99));  // higher bid wins
  CHECK_EQ(b.best_bid_size(), 500u);
  CHECK_EQ(b.best_ask_price(), hft::price_from_double(100.01));
  CHECK_EQ(b.best_ask_size(), 200u);

  // Consume the whole top bid level -> next-best (99.98) becomes the touch.
  b.apply(mk(hft::MsgType::kExecute, hft::Side::kBuy, hft::price_from_double(99.99), 500));
  CHECK_EQ(b.best_bid_price(), hft::price_from_double(99.98));
  CHECK_EQ(b.best_bid_size(), 300u);

  // Delete the remaining bid level -> bid side empty.
  b.apply(mk(hft::MsgType::kDelete, hft::Side::kBuy, hft::price_from_double(99.98), 0));
  CHECK(!b.has_bid());
}

HFT_TEST(imbalance_signs) {
  const std::int64_t base = hft::price_from_double(99.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<4096> b(base, tick);
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(100.00), 900));
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(100.01), 100));
  // Heavy bid -> positive imbalance ~ (900-100)/1000 = 0.8
  CHECK_NEAR(b.imbalance(), 0.8, 1e-9);
}

HFT_TEST(out_of_range_is_ignored_and_counted) {
  const std::int64_t base = hft::price_from_double(100.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  hft::BookView<16> b(base, tick);  // tiny 16-tick window
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kBuy, hft::price_from_double(50.00), 100));  // below
  b.apply(mk(hft::MsgType::kAdd, hft::Side::kSell, hft::price_from_double(200.00), 100)); // above
  CHECK(!b.has_bid());
  CHECK(!b.has_ask());
  CHECK_EQ(b.out_of_range(), 2u);
}

HFT_TEST(matches_reference_over_random_stream) {
  // Window: base 90.00, 4096 cent-ticks -> covers [90.00, 130.95]. Keep all
  // generated prices inside it so fast and reference books are comparable.
  const std::int64_t base = hft::price_from_double(90.00);
  const std::int64_t tick = hft::kPriceScale / 100;
  constexpr std::size_t N = 4096;
  hft::BookView<N> fast(base, tick);
  ReferenceBook ref;

  std::mt19937_64 rng(0xB00C);
  std::uniform_int_distribution<int> tick_off(0, static_cast<int>(N) - 1);
  std::uniform_int_distribution<int> which(0, 9);
  std::uniform_int_distribution<std::uint32_t> sz(1, 2000);

  for (int i = 0; i < 200'000; ++i) {
    const std::int64_t price = base + static_cast<std::int64_t>(tick_off(rng)) * tick;
    const hft::Side side = (rng() & 1) ? hft::Side::kBuy : hft::Side::kSell;
    const int roll = which(rng);
    hft::MsgType type;
    if (roll < 5) type = hft::MsgType::kAdd;          // 50% add
    else if (roll < 8) type = hft::MsgType::kExecute; // 30% execute
    else if (roll < 9) type = hft::MsgType::kCancel;  // 10% cancel
    else type = hft::MsgType::kDelete;                // 10% delete

    const hft::ItchMessage m = mk(type, side, price, sz(rng));
    fast.apply(m);
    ref.apply(m);

    if ((i & 0x3FF) == 0) check_match(fast, ref);  // compare periodically
  }
  check_match(fast, ref);  // and at the end
  CHECK_EQ(fast.out_of_range(), 0u);  // we kept everything in-window
}

HFT_TEST_MAIN()
